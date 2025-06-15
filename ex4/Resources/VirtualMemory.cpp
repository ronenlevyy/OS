#include "VirtualMemory.h"
#include "PhysicalMemory.h"
#include "MemoryConstants.h"

#include <climits>
#include <cstring>
#include <algorithm>

// Helper function to extract bits from an address
uint64_t extractBits(uint64_t address, int startBit, int numBits)
{
    uint64_t mask = (1ULL << numBits) - 1;
    return (address >> startBit) & mask;
}

// Helper function to get page number from virtual address
uint64_t getPageNumber(uint64_t virtualAddress)
{
    return virtualAddress >> OFFSET_WIDTH;
}

// Helper function to get offset from virtual address
uint64_t getOffset(uint64_t virtualAddress)
{
    return extractBits(virtualAddress, 0, OFFSET_WIDTH);
}

// Helper function to get table index at a specific level
uint64_t getTableIndex(uint64_t pageNumber, int level)
{
    int startBit = (TABLES_DEPTH - 1 - level) * OFFSET_WIDTH;
    return extractBits(pageNumber, startBit, OFFSET_WIDTH);
}

int calculateCyclicDistance(uint64_t p1, uint64_t p2)
{
    uint64_t diff = (p1 > p2) ? (p1 - p2) : (p2 - p1);
    uint64_t cyclic_diff = NUM_PAGES - diff;
    return (diff < cyclic_diff) ? diff : cyclic_diff;
}

// DFS function to traverse page table tree and find replacement candidates
void dfsTraverseTree(uint64_t targetFrameIndex, uint64_t targetPageIndex,
                     uint64_t currentFrameIndex, uint64_t currentPageIndex,
                     uint64_t parentEntry, uint64_t &evictParentEntry,
                     uint level, uint64_t &highestFrameSeen,
                     uint64_t &evictFrame, uint64_t &evictPage,
                     uint64_t &freeFrame)
{
    highestFrameSeen = std::max(currentFrameIndex, highestFrameSeen);

    if (level == TABLES_DEPTH)
    {
        uint64_t new_dist = calculateCyclicDistance(targetPageIndex, currentPageIndex);
        uint64_t old_dist = calculateCyclicDistance(targetPageIndex, evictPage);
        if (evictFrame == (uint64_t)-1 || new_dist > old_dist)
        {
            evictFrame = currentFrameIndex;
            evictPage = currentPageIndex;
            evictParentEntry = parentEntry;
        }
        return;
    }

    bool hasChildren = false;
    word_t word;

    for (uint64_t i = 0; i < PAGE_SIZE; i++)
    {
        uint64_t curr_address = currentFrameIndex*PAGE_SIZE + i;
        PMread(curr_address, &word);
        if (word != 0)
        {
            dfsTraverseTree(targetFrameIndex, targetPageIndex, word,
                            (currentPageIndex << OFFSET_WIDTH) + i, curr_address, evictParentEntry,
                            level + 1, highestFrameSeen, evictFrame,
                            evictPage, freeFrame);

            // if an empty table was found, we can return
            if (freeFrame != (uint64_t)-1)
            {
                return;
            }
            hasChildren = true;
        }
    }
    // if the current frame isn't used, and isn't the root frame or parent
    // frame
    if (!hasChildren && currentFrameIndex != 0 && currentFrameIndex != targetFrameIndex)
    {
        PMwrite(parentEntry, 0);
        freeFrame = currentFrameIndex;
    }
}

uint64_t findFrameToUse(uint64_t currentFrameIndex, uint64_t currentPageIndex)
{
    // First, look for an empty frame
    for (uint64_t frame = 1; frame < NUM_FRAMES; frame++)
    {
        bool isEmpty = true;
        for (uint64_t offset = 0; offset < PAGE_SIZE; offset++)
        {
            word_t value;
            PMread(frame * PAGE_SIZE + offset, &value);
            if (value != 0)
            {
                isEmpty = false;
                break;
            }
        }
        if (isEmpty)
        {
            return frame;
        }
    }
    uint64_t max_frame = 0;
    uint64_t empty_frame = (uint64_t)-1;
    uint64_t frame_to_evict = (uint64_t)-1;
    uint64_t page_to_evict = (uint64_t)-1;
    uint64_t evicted_parent_address = (uint64_t)-1;
    // dfsTraverseTree(currentFrameIndex, currentPageIndex, );

    if (empty_frame != (uint64_t)-1)
    {
        return empty_frame;
    }
    if (max_frame + 1 < NUM_FRAMES)
    {
        return max_frame + 1;
    }
    if (frame_to_evict != (uint64_t)-1)
    {
        PMevict(frame_to_evict, page_to_evict);
        PMwrite(evicted_parent_address, 0);
        return frame_to_evict;
    }
    return -1;
}

// Traverse page table hierarchy and create path if needed
uint64_t traversePageTable(uint64_t virtualAddress)
{
    uint64_t pageNumber = getPageNumber(virtualAddress);
    uint64_t currentFrame = 0;

    for (int i = 0; i < TABLES_DEPTH; i++)
    {
        uint64_t tableIndex = getTableIndex(pageNumber, i);
        uint64_t entryAddress = currentFrame * PAGE_SIZE + tableIndex;

        word_t entry;
        PMread(entryAddress, &entry);

        if (entry == 0)
        {

            // Need to allocate a new frame
            uint64_t newFrame = findFrameToUse(0, virtualAddress >> OFFSET_WIDTH);

            // Clear the new frame
            for (uint64_t offset = 0; offset < PAGE_SIZE; offset++)
            {
                PMwrite(newFrame * PAGE_SIZE + offset, 0);
            }

            // If this is the last level, restore the page from storage
            if (i == TABLES_DEPTH - 1)
            {
                uint64_t pageIndex = pageNumber;
                PMrestore(newFrame, pageIndex);
            }

            // Update the page table entry
            PMwrite(entryAddress, newFrame);
            currentFrame = newFrame;
        }
        else
        {
            currentFrame = entry;
        }
    }

    return currentFrame;
}

/*
 * Initialize the virtual memory
 */
void VMinitialize()
{
    for (uint64_t i = 0; i < PAGE_SIZE; i++)
    {
        PMwrite(i, 0);
    }
}

/* reads a word from the given virtual address
 * and puts its content in *value.
 *
 * returns 1 on success.
 * returns 0 on failure (if the address cannot be mapped to a physical
 * address for any reason)
 */
int VMread(uint64_t virtualAddress, word_t *value)
{
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE or value == nullptr)
    {
        return 0;
    }

    uint64_t frameIndex = traversePageTable(virtualAddress);
    if (frameIndex >= NUM_FRAMES)
    {
        return 0;
    }

    uint64_t offset = getOffset(virtualAddress);
    uint64_t physicalAddress = frameIndex * PAGE_SIZE + offset;
    if (physicalAddress >= RAM_SIZE)
    {
        return 0; // Prevent illegal access
    }

    PMread(physicalAddress, value);
    return 1;
}

/* writes a word to the given virtual address
 *
 * returns 1 on success.
 * returns 0 on failure (if the address cannot be mapped to a physical
 * address for any reason)
 */
int VMwrite(uint64_t virtualAddress, word_t value)
{
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE)
    {
        return 0;
    }

    uint64_t frameIndex = traversePageTable(virtualAddress);
    if (frameIndex >= NUM_FRAMES)
    {
        return 0;
    }

    uint64_t offset = getOffset(virtualAddress);
    uint64_t physicalAddress = frameIndex * PAGE_SIZE + offset;

    PMwrite(physicalAddress, value);
    return 1;
}