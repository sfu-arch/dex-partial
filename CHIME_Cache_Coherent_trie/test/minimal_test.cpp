/**
 * Minimal CHIME test - just one insert
 * Usage: ./minimal_test <node_count>
 */

#include "Tree.h"
#include "DSM.h"
#include <stdio.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: ./minimal_test <node_count>\n");
        printf("Example: ./minimal_test 2\n");
        return 1;
    }
    
    int node_count = atoi(argv[1]);
    printf("Starting minimal test with %d nodes\n", node_count);
    
    // Initialize DSM
    DSMConfig config;
    config.machineNR = node_count;
    config.threadNR = 1;  // Just 1 thread
    
    printf("Creating DSM...\n");
    DSM* dsm = DSM::getInstance(config);
    
    printf("Node %d: DSM created\n", dsm->getMyNodeID());
    
    // Register main thread
    bindCore(0);
    dsm->registerThread();
    printf("Node %d: Thread registered\n", dsm->getMyNodeID());
    
    // Create tree (only node 0 inits root)
    printf("Node %d: Creating tree...\n", dsm->getMyNodeID());
    Tree* tree = new Tree(dsm);
    printf("Node %d: Tree created!\n", dsm->getMyNodeID());
    
    // Barrier to sync
    dsm->barrier("init");
    printf("Node %d: Passed init barrier\n", dsm->getMyNodeID());
    
    // Only node 1 (compute node) tries to insert
    if (dsm->getMyNodeID() == 1) {
        printf("Node 1: About to insert key 0...\n");
        fflush(stdout);
        
        Key k = int2key(0);
        tree->insert(k, 100);
        
        printf("Node 1: Insert succeeded!\n");
        
        // Try to read it back
        printf("Node 1: About to search key 0...\n");
        Value v;
        tree->search(k, v);
        printf("Node 1: Search returned value=%lu\n", v);
    }
    
    dsm->barrier("done");
    printf("Node %d: Test complete!\n", dsm->getMyNodeID());
    
    return 0;
}
