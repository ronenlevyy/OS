#include "VirtualMemory.h"
#include "PhysicalMemory.h"
#include "MemoryConstants.h"

uint64_t extractBits(uint64_t address, int startBit, int numBits)
{
    uint64_t mask = (1ULL << numBits) - 1;
    return (address >> startBit) & mask;
}

uint64_t getPageNumber(uint64_t virtualAddress)
{
    return virtualAddress >> OFFSET_WIDTH;
}

uint64_t getPageOffset(uint64_t virtualAddress)
{
    return extractBits(virtualAddress, 0, OFFSET_WIDTH);
}

uint64_t getTableIndex(uint64_t pageIndex, int level)
{
    int startBit = (TABLES_DEPTH - 1 - level) * OFFSET_WIDTH;
    return extractBits(pageIndex, startBit, OFFSET_WIDTH);
}

int calculateCyclicDistance(uint64_t p1, uint64_t p2)
{
    uint64_t diff = (p1 > p2) ? (p1 - p2) : (p2 - p1);
    uint64_t cyclic_diff = NUM_PAGES - diff;
    return (diff < cyclic_diff) ? diff : cyclic_diff;
}

class FrameManager
{
private:
    struct FrameSearchResult
    {
        uint64_t maxFrame = 0;
        uint64_t freeFrame = (uint64_t)-1;
        uint64_t evictFrame = (uint64_t)-1;
        uint64_t evictPage = (uint64_t)-1;
        uint64_t evictParentEntry = (uint64_t)-1;
    };

    static void searchFrameTree(uint64_t targetFrame, uint64_t targetPage,
                                uint64_t currentFrame, uint64_t currentPage,
                                uint64_t parentEntry, FrameSearchResult &result,
                                uint64_t level)
    {

        result.maxFrame = (currentFrame > result.maxFrame) ? currentFrame : result.maxFrame;
        if (level == TABLES_DEPTH)
        {
            if (result.evictFrame == (uint64_t)-1 ||
                calculateCyclicDistance(targetPage, currentPage) >
                    calculateCyclicDistance(targetPage, result.evictPage))
            {
                result.evictFrame = currentFrame;
                result.evictPage = currentPage;
                result.evictParentEntry = parentEntry;
            }
            return;
        }

        bool hasChildren = false;
        word_t entry;

        for (uint64_t i = 0; i < PAGE_SIZE; i++)
        {
            uint64_t entryAddr = currentFrame * PAGE_SIZE + i;
            PMread(entryAddr, &entry);

            if (entry != 0)
            {
                searchFrameTree(targetFrame, targetPage, entry,
                                (currentPage << OFFSET_WIDTH) + i, entryAddr,
                                result, level + 1);

                if (result.freeFrame != (uint64_t)-1)
                    return;
                hasChildren = true;
            }
        }

        if (!hasChildren && currentFrame != 0 && currentFrame != targetFrame)
        {
            PMwrite(parentEntry, 0);
            result.freeFrame = currentFrame;
        }
    }

public:
    static uint64_t allocateFrame(uint64_t currentFrame, uint64_t currentPage)
    {
        FrameSearchResult result;
        searchFrameTree(currentFrame, currentPage, 0, 0, 0, result, 0);

        if (result.freeFrame != (uint64_t)-1)
            return result.freeFrame;
        if (result.maxFrame + 1 < NUM_FRAMES)
            return result.maxFrame + 1;
        if (result.evictFrame != (uint64_t)-1)
        {
            PMevict(result.evictFrame, result.evictPage);
            PMwrite(result.evictParentEntry, 0);
            return result.evictFrame;
        }
        return -1;
    }
};

uint64_t traverseAndAllocate(uint64_t virtualAddress)
{

    uint64_t pageNumber = getPageNumber(virtualAddress);
    uint64_t currentFrame = 0;
    word_t nextFrame;

    for (int level = 0; level < TABLES_DEPTH; level++)
    {
        uint64_t tableIndex = getTableIndex(pageNumber, level);
        uint64_t entryAddress = currentFrame * PAGE_SIZE + tableIndex;

        PMread(entryAddress, &nextFrame);

        if (nextFrame == 0)
        {
            uint64_t newFrame = FrameManager::allocateFrame(currentFrame, pageNumber);

            if (level == TABLES_DEPTH - 1)
            {
                PMrestore(newFrame, pageNumber);
            }
            else
            {
                for (uint64_t i = 0; i < PAGE_SIZE; i++)
                {
                    PMwrite(newFrame * PAGE_SIZE + i, 0);
                }
            }

            PMwrite(entryAddress, newFrame);
            nextFrame = newFrame;
        }
        currentFrame = nextFrame;
    }
    return currentFrame;
}

void VMinitialize()
{
    for (uint64_t i = 0; i < PAGE_SIZE; i++)
    {
        PMwrite(i, 0);
    }
}

int VMread(uint64_t virtualAddress, word_t *value)
{
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE || value == nullptr)
    {
        return 0;
    }

    uint64_t frame = traverseAndAllocate(virtualAddress);
    if (frame >= NUM_FRAMES)
        return 0;

    uint64_t offset = getPageOffset(virtualAddress);
    uint64_t physicalAddress = frame * PAGE_SIZE + offset;

    if (physicalAddress >= RAM_SIZE)
        return 0;

    PMread(physicalAddress, value);
    return 1;
}

int VMwrite(uint64_t virtualAddress, word_t value)
{
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE)
    {
        return 0;
    }

    uint64_t frame = traverseAndAllocate(virtualAddress);
    if (frame >= NUM_FRAMES)
        return 0;

    uint64_t offset = getPageOffset(virtualAddress);
    uint64_t physicalAddress = frame * PAGE_SIZE + offset;

    PMwrite(physicalAddress, value);
    return 1;
}