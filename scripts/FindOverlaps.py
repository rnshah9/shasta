#!/usr/bin/python3

import Nanopore2
import Nanopore2GetConfig
import sys

helpMessage="""
This uses the MinHash method to find overlapping reads.

Invoke without arguments.
"""

# Check that there are no arguments.
if not len(sys.argv)==1:
    print(helpMessage)
    exit(1)
    
# Read the config file.
config = Nanopore2GetConfig.getConfig()

# Initialize the assembler and access what we need.
a = Nanopore2.Assembler()
a.accessKmers()
a.accessMarkers()

# Do the computation.
a.findOverlaps(
    m = int(config['MinHash']['m']), 
    minHashIterationCount = int(config['MinHash']['minHashIterationCount']), 
    log2MinHashBucketCount = int(config['MinHash']['log2MinHashBucketCount']),
    maxBucketSize = int(config['MinHash']['maxBucketSize']),
    minFrequency = int(config['MinHash']['minFrequency']))

