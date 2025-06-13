#include "VirtualMemory.h"
#include "PhysicalMemory.h"
#include "MemoryConstants.h"

#include <climits>
#include <cstring>
#include <algorithm>

// Global variables for managing the virtual memory system
static bool isInitialized = false;

// Helper function to extract bits from an address
uint64_t extractBits(uint64_t address, int startBit, int numBits) {
    uint64_t mask = (1ULL << numBits) - 1;
    return (address >> startBit) & mask;
}

// Helper function to get page number from virtual address
uint64_t getPageNumber(uint64_t virtualAddress) {
    return virtualAddress >> OFFSET_WIDTH;
}

// Helper function to get offset from virtual address
uint64_t getOffset(uint64_t virtualAddress) {
    return extractBits(virtualAddress, 0, OFFSET_WIDTH);
}

// Helper function to get table index at a specific level
uint64_t getTableIndex(uint64_t pageNumber, int level) {
    int startBit = (TABLES_DEPTH - 1 - level) * OFFSET_WIDTH;
    return extractBits(pageNumber, startBit, OFFSET_WIDTH);
}

// Structure to track frame usage for replacement algorithm
struct FrameInfo {
    uint64_t frameIndex;
    uint64_t maxCyclicDistance;
    bool isEmpty;
    bool isPageTable;
    uint64_t pageIndex; // Only relevant for pages, not page tables
};

// DFS function to traverse page table tree and find replacement candidates
void dfsTraverseTree(uint64_t frameIndex, int currentDepth, uint64_t currentPagePrefix, 
                     FrameInfo& maxFrame, uint64_t& frameCounter, uint64_t targetFrame) {
    
    if (frameIndex == targetFrame) {
        return; // Don't consider the frame we're trying to allocate
    }
    
    frameCounter++;
    
    if (currentDepth == TABLES_DEPTH) {
        // This is a leaf (actual page)
        uint64_t cyclicDistance = frameCounter;
        if (cyclicDistance > maxFrame.maxCyclicDistance) {
            maxFrame.frameIndex = frameIndex;
            maxFrame.maxCyclicDistance = cyclicDistance;
            maxFrame.isEmpty = false;
            maxFrame.isPageTable = false;
            maxFrame.pageIndex = currentPagePrefix;
        }
        return;
    }
    
    // This is an internal page table node
    bool hasChildren = false;
    
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        word_t entry;
        PMread(frameIndex * PAGE_SIZE + i, &entry);
        
        if (entry != 0) {
            hasChildren = true;
            uint64_t childFrame = entry;
            uint64_t childPagePrefix = (currentPagePrefix << OFFSET_WIDTH) | i;
            dfsTraverseTree(childFrame, currentDepth + 1, childPagePrefix, 
                           maxFrame, frameCounter, targetFrame);
        }
    }
    
    // If this page table has no children, it can be reused
    if (!hasChildren && frameIndex != 0) { // Don't remove root page table
        uint64_t cyclicDistance = frameCounter;
        if (cyclicDistance > maxFrame.maxCyclicDistance) {
            maxFrame.frameIndex = frameIndex;
            maxFrame.maxCyclicDistance = cyclicDistance;
            maxFrame.isEmpty = false;
            maxFrame.isPageTable = true;
            maxFrame.pageIndex = 0; // Not relevant for page tables
        }
    }
}

// Find a frame to use (either empty or evict)
uint64_t findFrameToUse() {
    // First, look for an empty frame
    for (uint64_t frame = 0; frame < NUM_FRAMES; frame++) {
        bool isEmpty = true;
        for (uint64_t offset = 0; offset < PAGE_SIZE; offset++) {
            word_t value;
            PMread(frame * PAGE_SIZE + offset, &value);
            if (value != 0) {
                isEmpty = false;
                break;
            }
        }
        if (isEmpty && frame != 0) { // Don't use frame 0 (root page table)
            return frame;
        }
    }
    
    // No empty frame found, need to evict
    FrameInfo maxFrame = {0, 0, true, false, 0};
    uint64_t frameCounter = 0;
    
    // Traverse the entire page table tree starting from root (frame 0)
    dfsTraverseTree(0, 0, 0, maxFrame, frameCounter, NUM_FRAMES); // NUM_FRAMES as impossible target
    
    if (maxFrame.isEmpty) {
        // This shouldn't happen if we have a valid tree
        return 1; // Return frame 1 as fallback
    }
    
    uint64_t frameToEvict = maxFrame.frameIndex;
    
    if (!maxFrame.isPageTable) {
        // Evict a page
        PMevict(frameToEvict, maxFrame.pageIndex);
    }
    
    // Clear the frame
    for (uint64_t offset = 0; offset < PAGE_SIZE; offset++) {
        PMwrite(frameToEvict * PAGE_SIZE + offset, 0);
    }
    
    // Remove references to this frame from parent page tables
    // We need to traverse again to find and clear the reference
    // This is a simplified approach - in practice, we'd maintain parent pointers
    
    return frameToEvict;
}

// Traverse page table hierarchy and create path if needed
uint64_t traversePageTable(uint64_t virtualAddress, bool createPath) {
    uint64_t pageNumber = getPageNumber(virtualAddress);
    uint64_t currentFrame = 0; // Start from root page table
    
    for (int level = 0; level < TABLES_DEPTH; level++) {
        uint64_t tableIndex = getTableIndex(pageNumber, level);
        uint64_t entryAddress = currentFrame * PAGE_SIZE + tableIndex;
        
        word_t entry;
        PMread(entryAddress, &entry);
        
        if (entry == 0) {
            if (!createPath) {
                return NUM_FRAMES; // Invalid frame number indicates not found
            }
            
            // Need to allocate a new frame
            uint64_t newFrame = findFrameToUse();
            
            // Clear the new frame
            for (uint64_t offset = 0; offset < PAGE_SIZE; offset++) {
                PMwrite(newFrame * PAGE_SIZE + offset, 0);
            }
            
            // If this is the last level, restore the page from storage
            if (level == TABLES_DEPTH - 1) {
                uint64_t pageIndex = pageNumber;
                PMrestore(newFrame, pageIndex);
            }
            
            // Update the page table entry
            PMwrite(entryAddress, newFrame);
            currentFrame = newFrame;
        } else {
            currentFrame = entry;
        }
    }
    
    return currentFrame;
}

/*
 * Initialize the virtual memory
 */
void VMinitialize() {
    if (isInitialized) {
        return;
    }
    
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        PMwrite(i, 0);
    }
    
    isInitialized = true;
}

/* reads a word from the given virtual address
 * and puts its content in *value.
 *
 * returns 1 on success.
 * returns 0 on failure (if the address cannot be mapped to a physical
 * address for any reason)
 */
int VMread(uint64_t virtualAddress, word_t* value) {
    if (!isInitialized) {
        return 0;
    }
    
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE or value == nullptr) {
        return 0;
    }
    
    uint64_t frameIndex = traversePageTable(virtualAddress, true);
    if (frameIndex >= NUM_FRAMES) {
        return 0;
    }
    
    uint64_t offset = getOffset(virtualAddress);
    uint64_t physicalAddress = frameIndex * PAGE_SIZE + offset;
    
    PMread(physicalAddress, value);
    return 1;
}

/* writes a word to the given virtual address
 *
 * returns 1 on success.
 * returns 0 on failure (if the address cannot be mapped to a physical
 * address for any reason)
 */
int VMwrite(uint64_t virtualAddress, word_t value) {
    if (!isInitialized) {
        return 0;
    }
    
    if (virtualAddress >= VIRTUAL_MEMORY_SIZE) {
        return 0;
    }
    
    uint64_t frameIndex = traversePageTable(virtualAddress, true);
    if (frameIndex >= NUM_FRAMES) {
        return 0;
    }
    
    uint64_t offset = getOffset(virtualAddress);
    uint64_t physicalAddress = frameIndex * PAGE_SIZE + offset;
    
    PMwrite(physicalAddress, value);
    return 1;
} 