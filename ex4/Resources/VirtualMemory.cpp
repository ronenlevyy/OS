#include "VirtualMemory.h"
#include "PhysicalMemory.h"
#include "MemoryConstants.h"

/**
 * @brief Extracts a specified number of bits from an address starting at a given position
 * @param address The address to extract bits from
 * @param startBit The starting bit position (0-based)
 * @param numBits The number of bits to extract
 * @return The extracted bits as a uint64_t value
 */
uint64_t extractBits(uint64_t address, int startBit, int numBits)
{
    uint64_t mask = (1ULL << numBits) - 1;
    return (address >> startBit) & mask;
}

/**
 * @brief Gets the page number from a virtual address
 * @param virtualAddress The virtual address
 * @return The page number
 */
uint64_t getPageNumber(uint64_t virtualAddress)
{
    return virtualAddress >> OFFSET_WIDTH;
}

/**
 * @brief Gets the page offset from a virtual address
 * @param virtualAddress The virtual address
 * @return The page offset
 */
uint64_t getPageOffset(uint64_t virtualAddress)
{
    return extractBits(virtualAddress, 0, OFFSET_WIDTH);
}

/**
 * @brief Gets the table index for a specific level in the page table hierarchy
 * @param pageIndex The page index
 * @param level The level in the page table hierarchy
 * @return The table index for the specified level
 */
uint64_t getTableIndex(uint64_t pageIndex, int level)
{
    int startBit = (TABLES_DEPTH - 1 - level) * OFFSET_WIDTH;
    return extractBits(pageIndex, startBit, OFFSET_WIDTH);
}

/**
 * @brief Calculates the cyclic distance between two page numbers
 * @param p1 First page number
 * @param p2 Second page number
 * @return The minimum distance between the pages considering wraparound
 */
int calculateCyclicDistance(uint64_t p1, uint64_t p2)
{
    uint64_t diff = (p1 > p2) ? (p1 - p2) : (p2 - p1);
    uint64_t cyclic_diff = NUM_PAGES - diff;
    return (diff < cyclic_diff) ? diff : cyclic_diff;
}

/**
 * @brief FrameManager class handles frame allocation and management
 * 
 * This class implements the frame allocation strategy, including:
 * - Finding free frames
 * - Evicting pages when necessary
 * - Managing the frame tree structure
 */
class FrameManager
{
private:
    /**
     * @brief Structure to hold frame search results
     */
    struct FrameSearchResult
    {
        uint64_t maxFrame = 0;
        uint64_t freeFrame = (uint64_t)-1;
        uint64_t evictFrame = (uint64_t)-1;
        uint64_t evictPage = (uint64_t)-1;
        uint64_t evictParentEntry = (uint64_t)-1;
    };

    /**
     * @brief Recursively searches the frame tree for allocation opportunities
     * @param targetFrame The frame we're trying to allocate
     * @param targetPage The page we're trying to map
     * @param currentFrame Current frame being examined
     * @param currentPage Current page being examined
     * @param parentEntry Parent entry address
     * @param result Search results structure
     * @param level Current level in the page table hierarchy
     */
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
    /**
     * @brief Allocates a frame for a given page
     * @param currentFrame Current frame being processed
     * @param currentPage Current page being processed
     * @return The allocated frame number, or -1 if allocation failed
     */
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

/**
 * @brief Traverses the page table hierarchy and allocates frames as needed
 * @param virtualAddress The virtual address to map
 * @return The frame number where the page is mapped
 */
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