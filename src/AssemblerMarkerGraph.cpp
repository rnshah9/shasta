// Shasta.
#include "Assembler.hpp"
#include "AlignmentGraph.hpp"
#include "AssemblyGraph.hpp"
#include "ConsensusCaller.hpp"
#include "compressAlignment.hpp"
#include "Coverage.hpp"
#include "dset64-gccAtomic.hpp"
#include "PeakFinder.hpp"
#ifdef SHASTA_HTTP_SERVER
#include "LocalMarkerGraph.hpp"
#endif
#include "Reads.hpp"
#include "timestamp.hpp"
using namespace shasta;

// Spoa.
#include "spoa/spoa.hpp"

// Boost libraries.
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/dijkstra_shortest_paths_no_color_map.hpp>
#include <boost/graph/iteration_macros.hpp>
#include <boost/pending/disjoint_sets.hpp>

// Standard library.
#include "chrono.hpp"
#include <map>
#include <queue>



// Loop over all alignments in the read graph
// to create vertices of the global marker graph.
// Throw away vertices with coverage (number of markers)
// less than minCoverage or more than maxCoverage.
// Also throw away "bad" vertices - that is, vertices
// with more than one marker on the same oriented read.
void Assembler::createMarkerGraphVertices(

    // Minimum coverage (number of markers) for a vertex
    // of the marker graph to be kept.
    size_t minCoverage,

    // Maximum coverage (number of markers) for a vertex
    // of the marker graph to be kept.
    size_t maxCoverage,

    // Minimum coverage per strand (number of markers required
    // on each strand) for a vertex of the marker graph to be kept.
    uint64_t minCoveragePerStrand,

    // Flag that specifies whether to allow more than one marker on the
    // same oriented read id on a single marker graph vertex.
    bool allowDuplicateMarkers,

    // These two are used by PeakFinder in the automatic selection
    // of minCoverage when minCoverage is set to 0.
    double peakFinderMinAreaFraction,
    uint64_t peakFinderAreaStartIndex,

    // Number of threads. If zero, a number of threads equal to
    // the number of virtual processors is used.
    size_t threadCount
)
{

    // Flag to control debug output.
    // Only turn on for a very small test run.
    const bool debug = false;

    // using VertexId = MarkerGraph::VertexId;
    // using CompressedVertexId = CompressedGlobalMarkerGraphVertexId;

    const auto tBegin = steady_clock::now();
    cout << timestamp << "Begin computing marker graph vertices." << endl;

    // Check that we have what we need.
    reads->checkReadsAreOpen();
    reads->checkReadFlagsAreOpen();
    checkKmersAreOpen();
    checkMarkersAreOpen();
    checkAlignmentDataAreOpen();
    SHASTA_ASSERT(compressedAlignments.isOpen());


    // Store parameters so they are accessible to the threads.
    auto& data = createMarkerGraphVerticesData;
    data.allowDuplicateMarkers = allowDuplicateMarkers;
    data.minCoveragePerStrand = minCoveragePerStrand;

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Initialize computation of the global marker graph.
    data.orientedMarkerCount = markers.totalSize();

    data.disjointSetTable.createNew(
        largeDataName("tmp-DisjointSetTable"),
        largeDataPageSize);
    // DisjointSets data structure needs an additional 64 bits per entry, in order to implement
    // a lock-free, union-find operation. You can find more information in dset64-gccatomic.hpp.
    // Once the set representatives have been found, we have no need for these extra 64 bits per entry.
    //
    // We allocate twice as much space in data.disjointSetTable so that the underlying memory
    // can be used as an array of 128 bit integers of size data.orientedMarkerCount. This allows
    // us to compact the data in-place, there by reducing memory usage.
    data.disjointSetTable.reserveAndResize(data.orientedMarkerCount * 2);
    
    // Have DisjointSets use the memory allocated in and managed by data.disjointSetTable. 
    data.disjointSetsPointer = std::make_shared<DisjointSets>(
        reinterpret_cast<DisjointSets::Aint*>(data.disjointSetTable.begin()),
        data.orientedMarkerCount
    );



    // Update the disjoint set data structure for each alignment
    // in the read graph.
    cout << timestamp << "Disjoint set computation begins." << endl;
    size_t batchSize = 10000;
    setupLoadBalancing(readGraph.edges.size(), batchSize);
    runThreads(&Assembler::createMarkerGraphVerticesThreadFunction1, threadCount);
    cout << timestamp << "Disjoint set computation completed." << endl;



    // Find the disjoint set that each oriented marker was assigned to.
    // Iterate till each marker has its set representative populated in the parent (lower 64 bits)
    cout << timestamp << "Finding the disjoint set that each oriented marker was assigned to." << endl;
    uint64_t pass = 1;
    do {
        (data.disjointSetsPointer)->parentUpdated = 0;
        cout << "    " << timestamp << " Iteration  " << pass << endl;
        setupLoadBalancing(data.orientedMarkerCount, batchSize);
        runThreads(&Assembler::createMarkerGraphVerticesThreadFunction2, threadCount);
        cout << "    " << timestamp << " Updated parent of - " << (data.disjointSetsPointer)->parentUpdated << " entries." << endl;
        pass++;
    } while ((data.disjointSetsPointer)->parentUpdated > 0 && pass <= 10);

    if (pass > 10) {
        // This should never happen. Even in a highly parallel environment (128 threads), convergence happens
        // in 2 or 3 passes. It's definitely worth investigating if this ever happens.
        string errorMsg = "DisjointSets parent information did not converge in " + to_string(pass) + " iterations.";
        throw runtime_error(errorMsg);
    }
    

    cout << timestamp << "Verifying convergence of parent information." << endl;
    setupLoadBalancing(data.orientedMarkerCount, batchSize);
    runThreads(&Assembler::createMarkerGraphVerticesThreadFunction21, threadCount);
    cout << timestamp << "Done verifying convergence of parent information." << endl;


    // data.disjointSetTable now has the correct set representative for entry N at location 2*N.
    // That's because DisjointSets stores parent information in the lower 64 bits of the 128 bits
    // it uses for each entry. Since we only care about these bits, we can compact data.disjointSetTable
    // and free up half the memory.
    // This bit seems tricky to parallelize. It's not worth the effort as this is pretty fast as is.
    cout << timestamp << "Compacting the Disjoint Set data-structure." << endl;
    for(uint64_t i=0; i<data.orientedMarkerCount; i++) {
        data.disjointSetTable[i] = data.disjointSetTable[2*i];
    }
    data.disjointSetTable.resize(data.orientedMarkerCount);
    data.disjointSetTable.unreserve();
    cout << timestamp << "Done compacting the Disjoint Set data-structure." << endl;

    // Don't need the DisjointSets data-structure any more.
    data.disjointSetsPointer = 0;



    // Debug output.
    if(debug) {
        createMarkerGraphVerticesDebug1(0);
    }



    // Count the number of markers in each disjoint set
    // and store it in data.workArea.
    // We don't want to combine this with the previous block
    // because it would significantly increase the peak memory usage.
    // This way, we allocate data.workArea only after compacting 
    // data.disjointSetTable.
    cout << timestamp << "Counting the number of markers in each disjoint set." << endl;
    data.workArea.createNew(
        largeDataName("tmp-WorkArea"),
        largeDataPageSize);
    data.workArea.reserveAndResize(data.orientedMarkerCount);
    fill(data.workArea.begin(), data.workArea.end(), 0ULL);
    cout << "Processing " << data.orientedMarkerCount << " oriented markers." << endl;
    setupLoadBalancing(data.orientedMarkerCount, batchSize);
    runThreads(&Assembler::createMarkerGraphVerticesThreadFunction3, threadCount);



    // Debug output.
    if(debug) {
        ofstream out("WorkArea-initial-count.csv");
        for(MarkerId markerId=0; markerId<data.orientedMarkerCount; markerId++) {
            out << markerId << "," << data.workArea[markerId] << "\n";
        }

    }



    // At this point, data.workArea contains the number of oriented markers in
    // each disjoint set.
    // Compute a histogram of this distribution and write it to a csv file.
    {
        vector<uint64_t> histogram;
        for(MarkerGraph::VertexId i=0; i<data.orientedMarkerCount; i++) {
            const MarkerGraph::VertexId markerCount = data.workArea[i];
            if(markerCount == 0) {
                continue;
            }
            if(markerCount >= histogram.size()) {
                histogram.resize(markerCount+1, 0);
            }
            ++histogram[markerCount];
        }

        ofstream csv("DisjointSetsHistogram.csv");
        csv << "Coverage,Frequency\n";
        for(uint64_t coverage=0; coverage<histogram.size(); coverage++) {
            const uint64_t frequency = histogram[coverage];
            if(frequency) {
                csv << coverage << "," << frequency << "\n";
            }
        }

        if (minCoverage == 0) {
            try {
                shasta::PeakFinder p;
                p.findPeaks(histogram);
                minCoverage = p.findXCutoff(histogram, peakFinderMinAreaFraction, peakFinderAreaStartIndex);
                cout << "Automatically selected value of MarkerGraph.minCoverage "
                    "is " << minCoverage << endl;
            }
            catch (PeakFinderException& e){
                minCoverage = 5;
                cout <<
                    "Unable to automatically select MarkerGraph.minCoverage. "
                    "No significant cutoff found in disjoint sets size distribution. "
                    "Observed peak has percent total area of " << e.observedPercentArea << endl <<
                    "minPercentArea is " << e.minPercentArea << endl <<
                    "See DisjointSetsHistogram.csv."
                    "Using MarkerGraph.minCoverage = " << minCoverage << endl;
            }
        }
    }
    // Store the value of minCoverage actually used.
    assemblerInfo->markerGraphMinCoverageUsed = minCoverage;



    // At this point, data.workArea contains the number of oriented markers in
    // each disjoint set.
    // Replace it with a new numbering, counting only disjoint sets
    // with size not less than minCoverage and not greater than maxCoverage.
    // Note that this numbering is not yet the final vertex numbering,
    // as we will later remove "bad" vertices
    // (vertices with more than one marker on the same read).
    // This block is recursive and cannot be multithreaded.
    cout << timestamp << "Renumbering the disjoint sets." << endl;
    MarkerGraph::VertexId newDisjointSetId = 0ULL;
    for(MarkerGraph::VertexId oldDisjointSetId=0;
        oldDisjointSetId<data.orientedMarkerCount; ++oldDisjointSetId) {
        auto& w = data.workArea[oldDisjointSetId];
        const MarkerGraph::VertexId markerCount = w;
        if(markerCount<minCoverage || markerCount>maxCoverage) {
            w = MarkerGraph::invalidVertexId;
        } else {
            w = newDisjointSetId;
            ++newDisjointSetId;
        }
    }
    const auto disjointSetCount = newDisjointSetId;
    cout << "Kept " << disjointSetCount << " disjoint sets with coverage in the requested range." << endl;



    // Debug output.
    if(debug) {
        ofstream out("WorkArea-initial-renumbering.csv");
        for(MarkerId markerId=0; markerId<data.orientedMarkerCount; markerId++) {
            out << markerId << "," << data.workArea[markerId] << "\n";
        }
    }


    // Reassign vertices to disjoint sets using this new numbering.
    // Vertices assigned to no disjoint set will store MarkerGraph::invalidVertexId.
    // This could be multithreaded if necessary.
    cout << timestamp << "Assigning vertices to renumbered disjoint sets." << endl;
    for(MarkerGraph::VertexId markerId=0;
        markerId<data.orientedMarkerCount; ++markerId) {
        auto& d = data.disjointSetTable[markerId];
        const auto oldId = d;
        const auto newId = data.workArea[oldId];
        d = newId;
    }
    // We no longer need the workArea.
    data.workArea.remove();



    // Debug output.
    if(debug) {
        createMarkerGraphVerticesDebug1(1);
    }



    // At this point, data.disjointSetTable contains, for each oriented marker,
    // the disjoint set that the oriented marker is assigned to,
    // and all disjoint sets have a number of markers in the requested range.
    // Vertices not assigned to any disjoint set store a disjoint set
    // equal to MarkerGraph::invalidVertexId.



    // Gather the markers in each disjoint set.
    data.disjointSetMarkers.createNew(
        largeDataName("tmp-DisjointSetMarkers"),
        largeDataPageSize);
    cout << timestamp << "Gathering markers in disjoint sets, pass1." << endl;
    data.disjointSetMarkers.beginPass1(disjointSetCount);
    cout << timestamp << "Processing " << data.orientedMarkerCount << " oriented markers." << endl;
    setupLoadBalancing(data.orientedMarkerCount, batchSize);
    runThreads(&Assembler::createMarkerGraphVerticesThreadFunction4, threadCount);
    cout << timestamp << "Gathering markers in disjoint sets, pass2." << endl;
    data.disjointSetMarkers.beginPass2();
    cout << timestamp << "Processing " << data.orientedMarkerCount << " oriented markers." << endl;
    setupLoadBalancing(data.orientedMarkerCount, batchSize);
    runThreads(&Assembler::createMarkerGraphVerticesThreadFunction5, threadCount);
    data.disjointSetMarkers.endPass2();



    // Sort the markers in each disjoint set.
    cout << timestamp << "Sorting the markers in each disjoint set." << endl;
    setupLoadBalancing(disjointSetCount, batchSize);
    runThreads(&Assembler::createMarkerGraphVerticesThreadFunction6, threadCount);



    // Debug output.
    if(debug) {
        const uint64_t stage = 2;

        vector<uint64_t> histogram;
        for(uint64_t i=0; i<data.disjointSetMarkers.size(); i++) {
            const uint64_t size = data.disjointSetMarkers.size(i);
            if(histogram.size() <= size) {
                histogram.resize(size+1, 0);
            }
            ++histogram[size];
        }
        ofstream csv("Debug-DisjointSets-Histogram-" + to_string(stage) + ".csv");
        csv << "Size,Frequency\n";
        for(uint64_t size=0; size<histogram.size(); size++) {
            csv << size << "," << histogram[size] << "\n";
        }

    }



    // Flag "bad" disjoint sets for which we don't want to
    // create marker graph vertices. A disjoint set can be flagged
    // as bad for one of two reasons:
    // - It contains more than one marker on the same oriented read.
    // - It does not contain at least minCoveragePerStrand supporting
    //   oriented reads on each strand.
    data.isBadDisjointSet.createNew(
        largeDataName("tmp-IsBadDisjointSet"),
        largeDataPageSize);
    data.isBadDisjointSet.reserveAndResize(disjointSetCount);
    cout << timestamp << "Flagging bad disjoint sets." << endl;
    setupLoadBalancing(disjointSetCount, batchSize);
    runThreads(&Assembler::createMarkerGraphVerticesThreadFunction7, threadCount);
    const size_t badDisjointSetCount = std::count(
        data.isBadDisjointSet.begin(), data.isBadDisjointSet.end(), true);
    cout << "Found " << badDisjointSetCount << " disjoint sets "
        "with more than one marker on a single oriented read "
        "or with less than " << minCoveragePerStrand <<
        " supporting oriented reads on each strand." << endl;



    // Renumber the disjoint sets again, this time without counting the ones marked as bad.
    cout << timestamp << "Renumbering disjoint sets to remove the bad ones." << endl;
    data.workArea.createNew(
        largeDataName("tmp-WorkArea"),
        largeDataPageSize);
    data.workArea.reserveAndResize(disjointSetCount);
    newDisjointSetId = 0ULL;
    for(MarkerGraph::VertexId oldDisjointSetId=0;
        oldDisjointSetId<disjointSetCount; ++oldDisjointSetId) {
        auto& w = data.workArea[oldDisjointSetId];
        if(data.isBadDisjointSet[oldDisjointSetId]) {
            w = MarkerGraph::invalidVertexId;
        } else {
            w = newDisjointSetId;
            ++newDisjointSetId;
        }
    }
    SHASTA_ASSERT(newDisjointSetId + badDisjointSetCount == disjointSetCount);



    // Debug output.
    if(debug) {
        ofstream out("WorkArea-final-renumbering.csv");
        for(MarkerId markerId=0; markerId<disjointSetCount; markerId++) {
            out << markerId << "," << data.workArea[markerId] << "\n";
        }
    }


    // Compute the final disjoint set number for each marker.
    // That becomes the vertex id assigned to that marker.
    // This could be multithreaded.
    cout << timestamp << "Assigning vertex ids to markers." << endl;
    markerGraph.vertexTable.createNew(
        largeDataName("MarkerGraphVertexTable"),
        largeDataPageSize);
    markerGraph.vertexTable.reserveAndResize(data.orientedMarkerCount);
    for(MarkerGraph::VertexId markerId=0;
        markerId<data.orientedMarkerCount; ++markerId) {
        auto oldValue = data.disjointSetTable[markerId];
        if(oldValue == MarkerGraph::invalidVertexId) {
            markerGraph.vertexTable[markerId] = MarkerGraph::invalidCompressedVertexId;
        } else {
        	markerGraph.vertexTable[markerId] = data.workArea[oldValue];
        }
    }

    data.workArea.remove();
    data.disjointSetTable.remove();


    // Store the disjoint sets that are not marked bad.
    // Each corresponds to a vertex of the global marker graph.
    // This could be multithreaded.
    cout << timestamp << "Gathering the markers of each vertex of the marker graph." << endl;
    markerGraph.constructVertices();
    markerGraph.vertices().createNew(
        largeDataName("MarkerGraphVertices"),
        largeDataPageSize);
    for(MarkerGraph::VertexId oldDisjointSetId=0;
        oldDisjointSetId<disjointSetCount; ++oldDisjointSetId) {
        if(data.isBadDisjointSet[oldDisjointSetId]) {
            continue;
        }
        markerGraph.vertices().appendVector();
        const auto markers = data.disjointSetMarkers[oldDisjointSetId];
        for(const MarkerId markerId: markers) {
            markerGraph.vertices().append(markerId);
        }
    }
    markerGraph.vertices().unreserve();

    data.isBadDisjointSet.remove();
    data.disjointSetMarkers.remove();
    

    // Check that the data structures we created are consistent with each other.
    // This could be expensive. Remove when we know this code works.
    // cout << timestamp << "Checking marker graph vertices." << endl;
    // checkMarkerGraphVertices(minCoverage, maxCoverage);




    // Debug output.
    if(debug) {
        const uint64_t stage = 3;
        ofstream csv1("Debug-Vertices-" + to_string(stage) + ".csv");
        for(uint64_t i=0 ;i<markerGraph.vertices().size(); i++) {
            const auto v = markerGraph.vertices()[i];
            csv1 << v.size() << ",";
            for(const MarkerId markerId: v) {
                OrientedReadId orientedReadId;
                uint32_t ordinal;
                tie(orientedReadId, ordinal) = findMarkerId(markerId);
                csv1 << orientedReadId << "-" << ordinal << ",";
            }
            csv1 << "\n";
        }

        // Also create a histogram.
        vector<uint64_t> histogram;
        for(uint64_t i=0 ;i<markerGraph.vertices().size(); i++) {
            const auto v = markerGraph.vertices()[i];
            const uint64_t size = v.size();
            if(histogram.size() <= size) {
                histogram.resize(size+1, 0);
            }
            ++histogram[size];
        }
        ofstream csv3("Debug-Vertices-Histogram-" + to_string(stage) + ".csv");
        csv3 << "Size,Frequency\n";
        for(uint64_t size=0; size<histogram.size(); size++) {
            csv3 << size << "," << histogram[size] << "\n";
        }
    }



    const auto tEnd = steady_clock::now();
    const double tTotal = seconds(tEnd - tBegin);
    cout << timestamp << "Computation of global marker graph vertices ";
    cout << "completed in " << tTotal << " s." << endl;

}



void Assembler::createMarkerGraphVerticesThreadFunction1(size_t threadId)
{

    array<vector<MarkerWithOrdinal>, 2> markersSortedByKmerId;
    AlignmentGraph graph;
    Alignment alignment;
    AlignmentInfo alignmentInfo;

    auto& data = createMarkerGraphVerticesData;
    const std::shared_ptr<DisjointSets> disjointSetsPointer = data.disjointSetsPointer;

    const auto& storedAlignments = compressedAlignments;
    uint64_t alignmentId;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // We process read graph edges in pairs.
        // In each pair, the second edge is the reverse complement of the first.
        SHASTA_ASSERT((begin%2) == 0);
        SHASTA_ASSERT((end%2) == 0);

        for(size_t i=begin; i!=end; i+=2) {

            // Get the oriented read ids we want to align.
            array<OrientedReadId, 2> orientedReadIds;
            const ReadGraphEdge& readGraphEdge = readGraph.edges[i];
            alignmentId = readGraphEdge.alignmentId;

            // Check that the next edge is the reverse complement of
            // this edge.
            {
                const ReadGraphEdge& readGraphNextEdge = readGraph.edges[i + 1];
                array<OrientedReadId, 2> nextEdgeOrientedReadIds = readGraphNextEdge.orientedReadIds;
                nextEdgeOrientedReadIds[0].flipStrand();
                nextEdgeOrientedReadIds[1].flipStrand();
                SHASTA_ASSERT(nextEdgeOrientedReadIds == readGraphEdge.orientedReadIds);
            }

            // If the edge is flagged as crossing strands, skip it.
            if(readGraphEdge.crossesStrands) {
                continue;
            }

            // If the edge has an alignment flagged as inconsistent, skip it.
            if(readGraphEdge.hasInconsistentAlignment) {
                continue;
            }

            orientedReadIds = readGraphEdge.orientedReadIds;
            SHASTA_ASSERT(orientedReadIds[0] < orientedReadIds[1]);

            // If either of the reads is flagged chimeric, skip it.
            if( reads->getFlags(orientedReadIds[0].getReadId()).isChimeric ||
                reads->getFlags(orientedReadIds[1].getReadId()).isChimeric) {
                continue;
            }

            // Sanity check.
            SHASTA_ASSERT(alignmentData[alignmentId].info.isInReadGraph);

            // Decompress this alignment.
            span<const char> compressedAlignment = storedAlignments[alignmentId];
            shasta::decompress(compressedAlignment, alignment);


            // In the global marker graph, merge pairs
            // of aligned markers.
            for(const auto& p: alignment.ordinals) {
                const uint32_t ordinal0 = p[0];
                const uint32_t ordinal1 = p[1];
                const MarkerId markerId0 = getMarkerId(orientedReadIds[0], ordinal0);
                const MarkerId markerId1 = getMarkerId(orientedReadIds[1], ordinal1);
                SHASTA_ASSERT(markers.begin()[markerId0].kmerId == markers.begin()[markerId1].kmerId);
                disjointSetsPointer->unite(markerId0, markerId1);

                // Also merge the reverse complemented markers.
                // This guarantees that the marker graph remains invariant
                // under strand swap.
                disjointSetsPointer->unite(
                    findReverseComplement(markerId0),
                    findReverseComplement(markerId1));
            }
        }
    }

}



void Assembler::createMarkerGraphVerticesThreadFunction2(size_t threadId)
{
    DisjointSets& disjointSets = *createMarkerGraphVerticesData.disjointSetsPointer;
    
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerId i=begin; i!=end; ++i) {
            // Update parent information.
            disjointSets.find(i, true);
        }
    }
}

void Assembler::createMarkerGraphVerticesThreadFunction21(size_t threadId)
{
    DisjointSets& disjointSets = *createMarkerGraphVerticesData.disjointSetsPointer;
    const auto& disjointSetTable = createMarkerGraphVerticesData.disjointSetTable;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerId i=begin; i!=end; ++i) {
            // Verify that parent has been populated (I.e convergence happened after some iterations of
            // createMarkerGraphVerticesThreadFunction2)
            SHASTA_ASSERT(disjointSets.parent(i) == disjointSets.find(i));
            // Verify that reinterpreting DisjointSets data as a vector of uint64_t will work as expected.
            SHASTA_ASSERT(disjointSets.parent(i) == disjointSetTable[2*i]);
        }
    }
}


void Assembler::createMarkerGraphVerticesThreadFunction3(size_t threadId)
{
    const auto& disjointSetTable = createMarkerGraphVerticesData.disjointSetTable;
    auto& workArea = createMarkerGraphVerticesData.workArea;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerId i=begin; i!=end; ++i) {
            const uint64_t disjointSetId = disjointSetTable[i];

            // Increment the set size in a thread-safe way.
            __sync_fetch_and_add(&workArea[disjointSetId], 1ULL);
        }
    }
}



void Assembler::createMarkerGraphVerticesThreadFunction4(size_t threadId)
{
    createMarkerGraphVerticesThreadFunction45(4);
}



void Assembler::createMarkerGraphVerticesThreadFunction5(size_t threadId)
{
    createMarkerGraphVerticesThreadFunction45(5);
}



void Assembler::createMarkerGraphVerticesThreadFunction6(size_t threadId)
{
    auto& disjointSetMarkers = createMarkerGraphVerticesData.disjointSetMarkers;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerGraph::VertexId i=begin; i!=end; ++i) {
            auto markers = disjointSetMarkers[i];
            sort(markers.begin(), markers.end());
        }
    }
}



// Flag "bad" disjoint sets for which we don't want to
// create marker graph vertices. A disjoint set can be flagged
// as bad for one of two reasons:
// - It contains more than one marker on the same oriented read
//   (But this check if suppressed if allowDuplicateMarkers is set).
// - It does not contain at least minCoveragePerStrand supporting
//   oriented reads on each strand.
void Assembler::createMarkerGraphVerticesThreadFunction7(size_t threadId)
{
    const auto& disjointSetMarkers = createMarkerGraphVerticesData.disjointSetMarkers;
    auto& isBadDisjointSet = createMarkerGraphVerticesData.isBadDisjointSet;
    const auto allowDuplicateMarkers = createMarkerGraphVerticesData.allowDuplicateMarkers;
    const auto minCoveragePerStrand = createMarkerGraphVerticesData.minCoveragePerStrand;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerGraph::VertexId disjointSetId=begin; disjointSetId!=end; ++disjointSetId) {
            auto markers = disjointSetMarkers[disjointSetId];
            const size_t markerCount = markers.size();
            SHASTA_ASSERT(markerCount > 0);
            isBadDisjointSet[disjointSetId] = false;
            if(markerCount == 1) {
                if(1 < minCoveragePerStrand) {
                    isBadDisjointSet[disjointSetId] = true;
                }
                continue;
            }
            array<uint64_t, 2> countByStrand = {0, 0};
            for(size_t j=0; j<markerCount; j++) {
                const MarkerId& markerId = markers[j];
                OrientedReadId orientedReadId;
                tie(orientedReadId, ignore) = findMarkerId(markerId);
                ++countByStrand[orientedReadId.getStrand()];

                if((not allowDuplicateMarkers) and j > 0) {
                    const MarkerId& previousMarkerId = markers[j-1];
                    OrientedReadId previousOrientedReadId;
                    tie(previousOrientedReadId, ignore) = findMarkerId(previousMarkerId);
                    if(orientedReadId.getReadId() == previousOrientedReadId.getReadId()) {
                        isBadDisjointSet[disjointSetId] = true;
                        break;
                    }
                }
            }

            // If we did not flag it above due to more than one marker on the
            // same oriented read, check it for sufficient coverage on each strand.
            if(not isBadDisjointSet[disjointSetId]) {
                isBadDisjointSet[disjointSetId] =
                    (countByStrand[0] < minCoveragePerStrand) or
                    (countByStrand[1] < minCoveragePerStrand);
            }
        }
    }
}



void Assembler::createMarkerGraphVerticesThreadFunction45(int value)
{
    SHASTA_ASSERT(value==4 || value==5);
    const auto& disjointSetTable = createMarkerGraphVerticesData.disjointSetTable;
    auto& disjointSetMarkers = createMarkerGraphVerticesData.disjointSetMarkers;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerId i=begin; i!=end; ++i) {
            const uint64_t disjointSetId = disjointSetTable[i];
            if(disjointSetId == MarkerGraph::invalidVertexId) {
                continue;
            }
            if(value == 4) {
                disjointSetMarkers.incrementCountMultithreaded(disjointSetId);
            } else {
                disjointSetMarkers.storeMultithreaded(disjointSetId, i);
            }
        }
   }
}



void Assembler::createMarkerGraphVerticesDebug1(uint64_t stage)
{
    const auto& data = createMarkerGraphVerticesData;

    // Dump the disjoint sets table.
    ofstream csv1("Debug-DisjointSets-Table-" + to_string(stage) + ".csv");
    csv1 << "MarkerId,ReadId,Strand,Ordinal,DisjointSet\n";
    MarkerId markerIdCheck = 0;
    const ReadId readCount = getReads().readCount();
    for(ReadId readId=0; readId<readCount; readId++) {
        for(Strand strand=0; strand<2; strand++) {
            const OrientedReadId orientedReadId(readId, strand);
            const uint64_t thisOrientedReadMarkerCount = markers.size(orientedReadId.getValue());
            for(uint32_t ordinal=0; ordinal<thisOrientedReadMarkerCount; ordinal++) {
                const MarkerId markerId = getMarkerId(orientedReadId, ordinal);
                SHASTA_ASSERT(markerId == markerIdCheck++);
                csv1 <<
                    markerId << "," <<
                    readId << "," <<
                    strand << "," <<
                    ordinal << "," <<
                    data.disjointSetTable[markerId] << "\n";
            }
        }
    }

    // Gather the markers in disjoint sets and sort them so
    // the result is reproducible. This is expensive and can only be done like
    // this in debug code.
    std::map<MarkerId, vector<MarkerId> > m;
    for(MarkerId markerId=0; markerId<data.orientedMarkerCount; markerId++) {
        const auto disjointSetId = data.disjointSetTable[markerId];
        if(disjointSetId != MarkerGraph::invalidVertexId) {
            m[disjointSetId].push_back(markerId);
        }
    }
    vector< vector<MarkerId> > v;
    for(const auto& p: m) {
        v.push_back(p.second);
    }
    sort(v.begin(), v.end());
    ofstream csv2("Debug-DisjointSets" + to_string(stage) + ".csv");
    for(const auto& s: v) {
        csv2 << v.size() << ",";
        for(const MarkerId markerId: s) {
            OrientedReadId orientedReadId;
            uint32_t ordinal;
            tie(orientedReadId, ordinal) = findMarkerId(markerId);
            csv2 << orientedReadId << "-" << ordinal << ",";
        }
        csv2 << "\n";
    }

    // Also create a histogram.
    vector<uint64_t> histogram;
    for(const auto& s: v) {
        const uint64_t size = s.size();
        if(histogram.size() <= size) {
            histogram.resize(size+1, 0);
        }
        ++histogram[size];
    }
    ofstream csv3("Debug-DisjointSets-Histogram-" + to_string(stage) + ".csv");
    csv3 << "Size,Frequency\n";
    for(uint64_t size=0; size<histogram.size(); size++) {
        csv3 << size << "," << histogram[size] << "\n";
    }

}



// Check for consistency of markerGraph.vertexTable and markerGraph.vertices.
void Assembler::checkMarkerGraphVertices(
    size_t minCoverage,
    size_t maxCoverage)
{
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();
    SHASTA_ASSERT(markers.totalSize() == markerGraph.vertexTable.size());
    const MarkerId markerCount = markers.totalSize();



    // Dump everything. Only turn on for a very small test case.
    if(false) {
        ofstream out1("MarkerGraphVertexTable.csv");
        out1 << "MarkerId,VertexId\n";
        for(MarkerId markerId=0; markerId<markerCount; markerId++) {
            out1 << markerId << "," << markerGraph.vertexTable[markerId] << "\n";
        }
        ofstream out2("MarkerGraphVertices.csv");
        out1 << "VertexId,MarkerId\n";
        for(MarkerGraph::VertexId vertexId=0;
            vertexId<markerGraph.vertexCount(); vertexId++) {
            const auto markers = markerGraph.getVertexMarkerIds(vertexId);
            for(const MarkerId markerId: markers) {
                out2 << vertexId << "," << markerId << "\n";
            }
        }
    }



    for(MarkerGraph::VertexId vertexId=0;
        vertexId!=markerGraph.vertexCount(); vertexId++) {
        const auto markers = markerGraph.getVertexMarkerIds(vertexId);
        SHASTA_ASSERT(markers.size() >= minCoverage);
        SHASTA_ASSERT(markers.size() <= maxCoverage);
        for(const MarkerId markerId: markers) {
            if(markerGraph.vertexTable[markerId] != vertexId) {
                cout << "Failure at vertex " << vertexId << " marker " << markerId << endl;
            }
            SHASTA_ASSERT(markerGraph.vertexTable[markerId] == vertexId);
        }
    }
}



#if 0
void Assembler::createMarkerGraphVerticesThreadFunction3(size_t threadId)
{
    MarkerGraph::VertexId* workArea =
        createMarkerGraphVerticesData.workArea.begin();

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerId i=begin; i!=end; ++i) {
            const MarkerId rawVertexId = markerGraph.vertexTable[i];
            __sync_fetch_and_add(workArea + rawVertexId, 1ULL);
        }
    }
}



void Assembler::createMarkerGraphVerticesThreadFunction4(size_t threadId)
{
    MarkerGraph::VertexId* workArea =
        createMarkerGraphVerticesData.workArea.begin();
    const uint64_t maxValue = std::numeric_limits<uint64_t>::max();
    const uint64_t maxValueMinus1 = maxValue - 1ULL;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(MarkerId i=begin; i!=end; ++i) {
            const MarkerId rawVertexId = markerGraph.vertexTable[i];
            SHASTA_ASSERT(rawVertexId != maxValueMinus1);
            const MarkerId finalVertexId = workArea[rawVertexId];
            markerGraph.vertexTable[i] = finalVertexId;
        }
    }
}
#endif



void Assembler::accessMarkerGraphVertices(bool readWriteAccess)
{
    markerGraph.vertexTable.accessExisting(
        largeDataName("MarkerGraphVertexTable"), readWriteAccess);

    markerGraph.constructVertices();
    markerGraph.vertices().accessExisting(
        largeDataName("MarkerGraphVertices"), readWriteAccess);
}



void Assembler::checkMarkerGraphVerticesAreAvailable() const
{
    if(!markerGraph.vertices().isOpen() || !markerGraph.vertexTable.isOpen) {
        throw runtime_error("Vertices of the marker graph are not accessible.");
    }
}



// Find the vertex of the global marker graph that contains a given marker.
MarkerGraph::VertexId Assembler::getGlobalMarkerGraphVertex(
    ReadId readId,
    Strand strand,
    uint32_t ordinal) const
{
    return getGlobalMarkerGraphVertex(OrientedReadId(readId, strand), ordinal);

}
MarkerGraph::VertexId Assembler::getGlobalMarkerGraphVertex(
    OrientedReadId orientedReadId,
    uint32_t ordinal) const
{
    const MarkerId markerId =  getMarkerId(orientedReadId, ordinal);
    return markerGraph.vertexTable[markerId];
}



// Get pairs (ordinal, marker graph vertex id) for all markers of an oriented read.
// The pairs are returned sorted by ordinal.
void Assembler::getMarkerGraphVertices(
    OrientedReadId orientedReadId,
    vector< pair<uint32_t, MarkerGraph::VertexId> >& v)
{
    const uint32_t markerCount = uint32_t(markers.size(orientedReadId.getValue()));
    v.clear();
    for(uint32_t ordinal=0; ordinal<markerCount; ordinal++) {
        const MarkerGraph::VertexId vertexId =
            getGlobalMarkerGraphVertex(orientedReadId, ordinal);
        if(vertexId != MarkerGraph::invalidCompressedVertexId) {
            v.push_back(make_pair(ordinal, vertexId));
        }
    }
}



// Find the markers contained in a given vertex of the global marker graph.
// Returns the markers as tuples(read id, strand, ordinal).
vector< tuple<ReadId, Strand, uint32_t> >
    Assembler::getGlobalMarkerGraphVertexMarkers(
        MarkerGraph::VertexId globalMarkerGraphVertexId) const
{
    // Call the lower level function.
    vector< pair<OrientedReadId, uint32_t> > markers;
    getGlobalMarkerGraphVertexMarkers(globalMarkerGraphVertexId, markers);

    // Create the return vector.
    vector< tuple<ReadId, Strand, uint32_t> > returnVector;
    for(const auto& marker: markers) {
        const OrientedReadId orientedReadId = marker.first;
        const uint32_t ordinal = marker.second;
        returnVector.push_back(make_tuple(orientedReadId.getReadId(), orientedReadId.getStrand(), ordinal));
    }
    return returnVector;
}
void Assembler::getGlobalMarkerGraphVertexMarkers(
    MarkerGraph::VertexId vertexId,
    vector< pair<OrientedReadId, uint32_t> >& markers) const
{
    markers.clear();
    for(const MarkerId markerId: markerGraph.getVertexMarkerIds(vertexId)) {
        OrientedReadId orientedReadId;
        uint32_t ordinal;
        tie(orientedReadId, ordinal) = findMarkerId(markerId);
        markers.push_back(make_pair(orientedReadId, ordinal));
    }
}



// This version also returns the oriented read ids and ordinals
// that caused a child to be marked as such.
void Assembler::getGlobalMarkerGraphVertexChildren(
    MarkerGraph::VertexId vertexId,
    vector< pair<MarkerGraph::VertexId, vector<MarkerInterval> > >& children,
    vector< pair<MarkerGraph::VertexId, MarkerInterval> >& workArea
    ) const
{
    children.clear();
    workArea.clear();

    // Loop over the markers of this vertex.
    for(const MarkerId markerId: markerGraph.getVertexMarkerIds(vertexId)) {

        // Find the OrientedReadId and ordinal.
        MarkerInterval info;
        tie(info.orientedReadId, info.ordinals[0]) = findMarkerId(markerId);

        // Find the next marker that is contained in a vertex.
        const auto markerCount = markers.size(info.orientedReadId.getValue());
        for(info.ordinals[1]=info.ordinals[0]+1; info.ordinals[1]<markerCount; ++info.ordinals[1]) {

            // Find the vertex id.
            const MarkerId childMarkerId =  getMarkerId(info.orientedReadId, info.ordinals[1]);
            const MarkerGraph::VertexId childVertexId =
                markerGraph.vertexTable[childMarkerId];

            // If this marker correspond to a vertex, add it to our list.
            if( childVertexId!=MarkerGraph::invalidCompressedVertexId) {
                workArea.push_back(make_pair(childVertexId, info));
                break;
            }
        }

    }
    sort(workArea.begin(), workArea.end());



    // Now construct the children by gathering streaks of workArea entries
    // with the same child vertex id.
    for(auto streakBegin=workArea.begin(); streakBegin!=workArea.end(); ) {
        auto streakEnd = streakBegin + 1;
        for(;
            streakEnd!=workArea.end() && streakEnd->first==streakBegin->first;
            streakEnd++) {
        }
        children.resize(children.size() + 1);
        children.back().first = streakBegin->first;
        auto& v = children.back().second;
        for(auto it=streakBegin; it!=streakEnd; it++) {
            v.push_back(it->second);
        }

        // Process the next streak.
        streakBegin = streakEnd;
    }
}



// Given two marker graph vertices, get the marker intervals
// that a possible edge between the two vertices would have.
void Assembler::getMarkerIntervals(
    MarkerGraph::VertexId v0,
    MarkerGraph::VertexId v1,
    vector<MarkerInterval>& markerIntervals
    ) const
{
    // Start with no marker intervals.
    markerIntervals.clear();

    // Loop over the markers of vertex v0.
    const span<const MarkerId> markerIds0 = markerGraph.getVertexMarkerIds(v0);
    for(const MarkerId markerId0: markerIds0) {

        // Find the OrientedReadId and ordinal.
        OrientedReadId orientedReadId;
        uint32_t ordinal0;
        tie(orientedReadId, ordinal0) = findMarkerId(markerId0);

        // Find the next marker for this oriented read that is contained in a vertex.
        const span<const CompressedMarker> markers1 = markers[orientedReadId.getValue()];
        uint32_t ordinal1 = ordinal0 + 1;
        for(; ordinal1<markers1.size(); ++ordinal1) {

            // Find the vertex id.
            const MarkerId markerId1 = getMarkerId(orientedReadId, ordinal1);
            const MarkerGraph::VertexId v1Check = markerGraph.vertexTable[markerId1];
            if(v1Check == MarkerGraph::invalidCompressedVertexId) {
                // There is no vertex on this marker. Try the next ordinal.
                continue;
            }

            // If the next vertex is v1, this oriented read generates a marker interval.
            if(v1Check == v1) {
                markerIntervals.push_back(MarkerInterval(orientedReadId, ordinal0, ordinal1));
            }

            // We found the next vertex for this OrientedReadId.
            break;
        }

    }

    sort(markerIntervals.begin(), markerIntervals.end());
}



// Find the reverse complement of each marker graph vertex.
void Assembler::findMarkerGraphReverseComplementVertices(size_t threadCount)
{
    cout << timestamp << "Begin findMarkerGraphReverseComplementVertices."
        << endl;

    // Check that we have what we need.
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Get the number of vertices in the marker graph.
    using VertexId = MarkerGraph::VertexId;
    const VertexId vertexCount = markerGraph.vertexCount();

    // Allocate the vector to hold the reverse complemented
    // vertex id for each vertex.
    if(not markerGraph.reverseComplementVertex.isOpen) {
        markerGraph.reverseComplementVertex.createNew(
            largeDataName("MarkerGraphReverseComplementeVertex"),
            largeDataPageSize);
    }
    markerGraph.reverseComplementVertex.resize(vertexCount);

    // Check each vertex.
    setupLoadBalancing(vertexCount, 10000);
    runThreads(&Assembler::findMarkerGraphReverseComplementVerticesThreadFunction1,
        threadCount);

    // Check that the reverse complement of the reverse complement of a
    // vertex is the vertex itself.
    setupLoadBalancing(vertexCount, 10000);
    runThreads(&Assembler::findMarkerGraphReverseComplementVerticesThreadFunction2,
        threadCount);
    cout << timestamp << "Begin findMarkerGraphReverseComplementVertices." << endl;

}



void Assembler::findMarkerGraphReverseComplementVerticesThreadFunction1(size_t threadId)
{
    using VertexId = MarkerGraph::VertexId;

    // Loop over batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // Loop over vertices in this batch.
        for (VertexId vertexId=begin; vertexId!=end; vertexId++) {

            // Get the markers of this vertex.
            const span<MarkerId> vertexMarkers = markerGraph.getVertexMarkerIds(vertexId);
            SHASTA_ASSERT(vertexMarkers.size() > 0);

            // Get the first marker of this vertex.
            const MarkerId firstMarkerId = vertexMarkers[0];

            /// Find the reverse complemented marker.
            const MarkerId firstMarkerIdReverseComplement = findReverseComplement(firstMarkerId);

            // Find the corresponding vertex.
            const VertexId vertexIdReverseComplement = markerGraph.vertexTable[firstMarkerIdReverseComplement];
            SHASTA_ASSERT(vertexIdReverseComplement != MarkerGraph::invalidCompressedVertexId);

            // Now check that we get the same reverse complement vertex for all markers.
            for(const MarkerId markerId: vertexMarkers) {
                const MarkerId markerIdReverseComplement = findReverseComplement(markerId);
                SHASTA_ASSERT(markerGraph.vertexTable[markerIdReverseComplement] == vertexIdReverseComplement);
            }

            // Store the reverse complement vertex.
            markerGraph.reverseComplementVertex[vertexId] = vertexIdReverseComplement;
        }
    }
}



void Assembler::findMarkerGraphReverseComplementVerticesThreadFunction2(size_t threadId)
{
    using VertexId = MarkerGraph::VertexId;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        for (VertexId vertexId=begin; vertexId!=end; vertexId++) {
            const VertexId vertexIdReverseComplement =
                markerGraph.reverseComplementVertex[vertexId];
            SHASTA_ASSERT(
                markerGraph.reverseComplementVertex[vertexIdReverseComplement] == vertexId);
        }
    }
}



void Assembler::accessMarkerGraphReverseComplementVertex(bool readWriteAccess)
{
    markerGraph.reverseComplementVertex.accessExisting(
        largeDataName("MarkerGraphReverseComplementeVertex"),
        readWriteAccess);
}



// Find the reverse complement of each marker graph edge.
void Assembler::findMarkerGraphReverseComplementEdges(size_t threadCount)
{
    cout << timestamp << "Begin findMarkerGraphReverseComplementEdges." << endl;

    // Check that we have what we need.
    checkMarkerGraphVerticesAreAvailable();
    checkMarkerGraphEdgesIsOpen();
    SHASTA_ASSERT(markerGraph.reverseComplementVertex.isOpen);

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Get the number of edges in the marker graph.
    using EdgeId = MarkerGraph::EdgeId;
    const EdgeId edgeCount = markerGraph.edges.size();

    // Allocate the vector to hold the reverse complemented
    // edge id for each edge.
    markerGraph.reverseComplementEdge.createNew(
        largeDataName("MarkerGraphReverseComplementeEdge"), largeDataPageSize);
    markerGraph.reverseComplementEdge.resize(edgeCount);

    // Check all marker graph edges.
    setupLoadBalancing(edgeCount, 10000);
    runThreads(&Assembler::findMarkerGraphReverseComplementEdgesThreadFunction1,
        threadCount);

    // Check that the reverse complement of the reverse complement of an
    // edge is the edge itself.
    setupLoadBalancing(edgeCount, 10000);
    runThreads(&Assembler::findMarkerGraphReverseComplementEdgesThreadFunction2,
        threadCount);

    cout << timestamp << "End findMarkerGraphReverseComplementEdges." << endl;

}



void Assembler::findMarkerGraphReverseComplementEdgesThreadFunction1(size_t threadId)
{
    using VertexId = MarkerGraph::VertexId;
    using EdgeId = MarkerGraph::EdgeId;

    vector<MarkerInterval> resortedMarkers;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(EdgeId edgeId=begin; edgeId!=end; edgeId++) {
            const MarkerGraph::Edge& edge = markerGraph.edges[edgeId];
            const VertexId v0 = edge.source;
            const VertexId v1 = edge.target;
            const VertexId v0Rc = markerGraph.reverseComplementVertex[v0];
            const VertexId v1Rc = markerGraph.reverseComplementVertex[v1];
            const span<MarkerInterval> markerIntervals = markerGraph.edgeMarkerIntervals[edgeId];

            // This code need to be resilient to two situations:
            // - Vertices with more than one marker on the same oriented read id
            //   (these can occur when using --MarkerGraph.allowDuplicateMarkers).
            // - Parallel edges (more than one edge with the same source and
            //   target vertex). These can occur when using createMarkerGraphEdgesStrict.

#if 0
            cout << "Looking for reverse complement of " << edgeId << " " <<
                v0 << "->" << v1 << endl;
            cout << "Looking for " << v1Rc << "->" << v0Rc << endl;
#endif


            // Look for an edge v1rc->v0rc with identical marker intervals,
            // after reverse complementing.
            const span<Uint40> v1rcOutEdges = markerGraph.edgesBySource[v1Rc];
            bool found = false;
            for(const Uint40 edgeIdRc: v1rcOutEdges) {
                const MarkerGraph::Edge& edgeRc = markerGraph.edges[edgeIdRc];
                SHASTA_ASSERT(edgeRc.source == v1Rc);
                if(edgeRc.target != v0Rc) {
                    continue;
                }

                // cout << "Found " << edgeIdRc << endl;

                // Gather the reverse complemented marker intervals,
                // and reverse complement them.
                resortedMarkers.clear();
                const span<MarkerInterval> markerIntervalsRc = markerGraph.edgeMarkerIntervals[edgeIdRc];
                for(MarkerInterval markerInterval: markerIntervalsRc) {
                    const uint32_t markerCount = uint32_t(markers.size(markerInterval.orientedReadId.getValue()));
                    markerInterval.orientedReadId.flipStrand();
                    markerInterval.ordinals[0] = markerCount - 1 - markerInterval.ordinals[0];
                    markerInterval.ordinals[1] = markerCount - 1 - markerInterval.ordinals[1];
                    swap(markerInterval.ordinals[0], markerInterval.ordinals[1]);
                    resortedMarkers.push_back(markerInterval);
                }
                sort(resortedMarkers.begin(), resortedMarkers.end());
                const span<MarkerInterval> resortedMarkersSpan(&resortedMarkers[0], &resortedMarkers[0]+resortedMarkers.size());

                if(resortedMarkersSpan == markerIntervals) {
                    // They are equal to what we were looking for.
                    // We found the reverse complement edge.
                    markerGraph.reverseComplementEdge[edgeId] = edgeIdRc;
                    found = true;
                    // cout << "Found with consistent marker intervals." << endl;
                    break;
                }
            }
            if(not found) {
                const string message = "Unable to locate reverse complement of marker graph edge " +
                    to_string(edgeId) + " " + to_string(v0) + "->" + to_string(v1);
                cout << message << endl;
                cout << "Writing marker graph details to csv files." <<endl;
                debugWriteMarkerGraph();
                throw runtime_error(message);
            }
        }
    }
}



// Check that the reverse complement of the reverse complement of an
// edge is the edge itself.
void Assembler::findMarkerGraphReverseComplementEdgesThreadFunction2(size_t threadId)
{
    using EdgeId = MarkerGraph::EdgeId;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for(EdgeId edgeId=begin; edgeId!=end; edgeId++) {
            const EdgeId edgeIdReverseComplement =
                markerGraph.reverseComplementEdge[edgeId];
            if(markerGraph.reverseComplementEdge[edgeIdReverseComplement] != edgeId) {
                const string message = "Reverse complement edge check failed at edge " +
                    to_string(edgeId) + ": " +
                    to_string(edgeIdReverseComplement) + " " +
                    to_string(markerGraph.reverseComplementEdge[edgeIdReverseComplement]);
                cout << message << endl;
                cout << "Writing marker graph details to csv files." <<endl;
                debugWriteMarkerGraph();
                throw runtime_error(message);
            }
        }
    }
}



void Assembler::accessMarkerGraphReverseComplementEdge()
{
    markerGraph.reverseComplementEdge.accessExistingReadOnly(
        largeDataName("MarkerGraphReverseComplementeEdge"));
}



// Check that the marker graph is strand symmetric.
// This can only be called after both findMarkerGraphReverseComplementVertices
// and findMarkerGraphReverseComplementEdges have been called,
// as it requires markerGraph.reverseComplementVertex
// and markerGraph.reverseComplementEdge.
void Assembler::checkMarkerGraphIsStrandSymmetric(size_t threadCount)
{
    // cout << timestamp << "Begin checkMarkerGraphIsStrandSymmetric." << endl;

    // Check that we have what we need.
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();
    checkMarkerGraphEdgesIsOpen();
    SHASTA_ASSERT(markerGraph.reverseComplementVertex.isOpen);

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Check the vertices.
    using VertexId = MarkerGraph::VertexId;
    const VertexId vertexCount = markerGraph.vertexCount();
    setupLoadBalancing(vertexCount, 10000);
    runThreads(&Assembler::checkMarkerGraphIsStrandSymmetricThreadFunction1, threadCount);

    // Check the edges.
    using EdgeId = MarkerGraph::EdgeId;
    const EdgeId edgeCount = markerGraph.edges.size();
    setupLoadBalancing(edgeCount, 10000);
    runThreads(&Assembler::checkMarkerGraphIsStrandSymmetricThreadFunction2, threadCount);

    // cout << timestamp << "End checkMarkerGraphIsStrandSymmetric." << endl;
}



// Check the vertices.
void Assembler::checkMarkerGraphIsStrandSymmetricThreadFunction1(size_t threadId)
{

    // Loop over all batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // Loop over all marker graph vertices in this batch.
        for (MarkerGraph::VertexId v0=begin; v0!=end; v0++) {

            // Find v1, the reverse complement of v0
            const MarkerGraph::VertexId v1 = markerGraph.reverseComplementVertex[v0];

            // Check that v2, the reverse complement of v1, is v0.
            const MarkerGraph::VertexId v2 = markerGraph.reverseComplementVertex[v1];
            SHASTA_ASSERT(v2 == v0);

            // This can happen in exceptional cases, so this check is commented out.
            // SHASTA_ASSERT(v1 != v0);

            // Check that v0 and v1 have the same number of markers.
            const span<MarkerId> markers0 = markerGraph.getVertexMarkerIds(v0);
            const span<MarkerId> markers1 = markerGraph.getVertexMarkerIds(v1);
            SHASTA_ASSERT(markers0.size() == markers1.size());

            // Check all the markers of v0.
            for (size_t i = 0; i < markers0.size(); i++) {
                const MarkerId markerId0 = markers0[i];
                const MarkerId markerId1 = findReverseComplement(markerId0);
                SHASTA_ASSERT(markerGraph.vertexTable[markerId0] == v0);
                SHASTA_ASSERT(markerGraph.vertexTable[markerId1] == v1);
            }
        }
    }
}



// Check the edges.
void Assembler::checkMarkerGraphIsStrandSymmetricThreadFunction2(size_t threadId)
{
    using EdgeId = MarkerGraph::EdgeId;

    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        for (EdgeId e0=begin; e0!=end; e0++) {
            const EdgeId e1 = markerGraph.reverseComplementEdge[e0];
            const EdgeId e2 = markerGraph.reverseComplementEdge[e1];
            SHASTA_ASSERT(e2 == e0);
            SHASTA_ASSERT(e1 != e0);

            const MarkerGraph::Edge& edge0 = markerGraph.edges[e0];
            const MarkerGraph::Edge& edge1 = markerGraph.edges[e1];
            SHASTA_ASSERT(edge0.coverage == edge1.coverage);
            SHASTA_ASSERT(
                edge0.wasRemovedByTransitiveReduction
                == edge1.wasRemovedByTransitiveReduction);
            SHASTA_ASSERT(edge0.wasPruned == edge1.wasPruned);
            SHASTA_ASSERT(edge0.isSuperBubbleEdge == edge1.isSuperBubbleEdge);

#if 0
            // This portion does not work if parallel edges are present,
            // which can happen in assembly mode 1.
            const VertexId v0 = edge0.source;
            const VertexId v1 = edge0.target;
            const VertexId v0rc = markerGraph.reverseComplementVertex[v0];
            const VertexId v1rc = markerGraph.reverseComplementVertex[v1];
            const EdgeId e0rc = markerGraph.findEdgeId(v1rc, v0rc);
            SHASTA_ASSERT(e0rc == e1);
#endif

#if 0
            // This check does not work correctly when --MarkerGraph.allowDuplicateMarkers.
            // An equivalent check was done in findMarkerGraphReverseComplementVertices.
            const span<MarkerInterval> markerIntervals0 =
                markerGraph.edgeMarkerIntervals[e0];
            const span<MarkerInterval> markerIntervals1 =
                markerGraph.edgeMarkerIntervals[e1];
            SHASTA_ASSERT(markerIntervals0.size() == markerIntervals1.size());
            for (size_t i=0; i<markerIntervals0.size(); i++) {
                const MarkerInterval& markerInterval0 = markerIntervals0[i];
                const MarkerInterval& markerInterval1 = markerIntervals1[i];
                SHASTA_ASSERT(
                    markerInterval0.orientedReadId.getReadId()
                    == markerInterval1.orientedReadId.getReadId());
                SHASTA_ASSERT(
                    markerInterval0.orientedReadId.getStrand()
                    == 1 - markerInterval1.orientedReadId.getStrand());
                const uint32_t markerCount = uint32_t(
                    markers.size(markerInterval0.orientedReadId.getValue()));
                SHASTA_ASSERT(
                    markerInterval0.ordinals[0]
                    == markerCount - 1 - markerInterval1.ordinals[1]);
                SHASTA_ASSERT(
                    markerInterval0.ordinals[1]
                    == markerCount - 1 - markerInterval1.ordinals[0]);
            }
#endif
        }
    }
}



// Return true if a vertex of the global marker graph has more than
// one marker for at least one oriented read id.
bool Assembler::isBadMarkerGraphVertex(MarkerGraph::VertexId vertexId) const
{
    // Get the markers of this vertex.
    const auto& vertexMarkerIds = markerGraph.getVertexMarkerIds(vertexId);

    // The markers are sorted by OrientedReadId, so we can just check each
    // consecutive pairs.
    for(size_t i=1; i<vertexMarkerIds.size(); i++) {
        const MarkerId markerId0 = vertexMarkerIds[i-1];
        const MarkerId markerId1 = vertexMarkerIds[i];
        OrientedReadId orientedReadId0;
        OrientedReadId orientedReadId1;
        tie(orientedReadId0, ignore) = findMarkerId(markerId0);
        tie(orientedReadId1, ignore) = findMarkerId(markerId1);
        if(orientedReadId0 == orientedReadId1) {
            return true;
        }
    }
    return false;
}



// Write a csv file with information on all marker graph vertices for which
// isBadMarkerGraphVertex returns true.
void Assembler::writeBadMarkerGraphVertices() const
{
    checkMarkerGraphVerticesAreAvailable();
    ofstream csv("BadMarkerGraphVertices.csv");
    csv << "VertexId,FirstOrientedReadId,FirstOrdinal,Coverage,DuplicateCoverage\n";

    uint64_t badVertexCount = 0;
    for(MarkerGraph::VertexId vertexId=0; vertexId!=markerGraph.vertexCount(); vertexId++) {
        if(not isBadMarkerGraphVertex(vertexId)) {
            continue;
        }
        ++badVertexCount;

        // Get the markers.
        const span<const MarkerId> markerIds = markerGraph.getVertexMarkerIds(vertexId);
        SHASTA_ASSERT(markerIds.size() > 0);

        // Get information on the first marker, to output to the csv file.
        const MarkerId firstMarkerId = markerIds[0];
        OrientedReadId firstOrientedReadId;
        uint32_t firstOrdinal;
        tie(firstOrientedReadId, firstOrdinal) = findMarkerId(firstMarkerId);

        // Count the number of duplicate markers.
        std::map<OrientedReadId, uint64_t> frequencyMap;
        for(const MarkerId markerId: markerIds) {
            OrientedReadId orientedReadId;
            tie(orientedReadId, ignore) = findMarkerId(markerId);
            ++frequencyMap[orientedReadId];
        }
        uint64_t duplicateCoverage = 0;
        for(const auto& p: frequencyMap) {
            const uint64_t frequency = p.second;
            if(frequency > 1) {
                duplicateCoverage += frequency;
            }
        }

        // Write.
        csv << vertexId << ",";
        csv << firstOrientedReadId << ",";
        csv << firstOrdinal << ",";
        csv << markerIds.size() << ",";
        csv << duplicateCoverage << "\n";
    }

    cout << "Found " << badVertexCount << " bad marker graph vertices." << endl;
}



// Compute marker graph vertex coverage statistics by KmerId.
void Assembler::vertexCoverageStatisticsByKmerId() const
{
    // Check that we have what we need.
    checkKmersAreOpen();
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();

    const uint64_t k = assemblerInfo->k;

    // For each KmerId, maintain a histogram by coverage.
    vector< vector<uint64_t> > histogram(kmerTable.size());

    // Loop over all marker graph vertices.
    for(MarkerGraph::VertexId vertexId=0; vertexId!=markerGraph.vertexCount(); vertexId++) {

        // Get the markers for this vertex.
        const span<const MarkerId> markerIds = markerGraph.getVertexMarkerIds(vertexId);
        const uint64_t coverage = markerIds.size();
        SHASTA_ASSERT(coverage > 0);

        // Find the KmerId.
        const MarkerId firstMarkerId = markerIds.front();
        const CompressedMarker& compressedMarker = markers.begin()[firstMarkerId];
        const KmerId kmerId = compressedMarker.kmerId;

        // Increment the histogram.
        SHASTA_ASSERT(kmerId < histogram.size());
        vector<uint64_t>& h = histogram[kmerId];
        if(h.size() <= coverage) {
            h.resize(coverage + 1, 0ULL);
        }
        ++h[coverage];
    }



    // Find the maximum histogram size for any k-mer.
    uint64_t hMaxSize = 0ULL;
    for(uint64_t kmerId=0; kmerId<kmerTable.size(); kmerId++) {
        if(not kmerTable[kmerId].isMarker) {
            continue;
        }
        if(not kmerTable[kmerId].isRleKmer) {
            continue;
        }
        const vector<uint64_t>& h = histogram[kmerId];
        hMaxSize = max(hMaxSize, uint64_t(h.size()));
    }



    // Write it out.
    ofstream csv("VertexCoverageByKmerId.csv");
    csv << "Kmer,Total,";
    for(uint64_t coverage=1; coverage<hMaxSize; coverage++) {
        csv << coverage << ",";
    }
    csv << "\n";
    for(uint64_t kmerId=0; kmerId<kmerTable.size(); kmerId++) {
        if(not kmerTable[kmerId].isMarker) {
            continue;
        }
        if(not kmerTable[kmerId].isRleKmer) {
            continue;
        }
        const Kmer kmer(kmerId, k);

        // Compute the total number of markers with this k-mer
        // that are associated with a vertex.
        const vector<uint64_t>& h = histogram[kmerId];
        uint64_t totalMarkerCount = 0ULL;
        for(uint64_t coverage=1; coverage<hMaxSize; coverage++) {
            uint64_t vertexCount = 0;
            if(coverage < h.size()) {
                vertexCount = h[coverage];
            }
            const uint64_t markerCount = coverage * vertexCount;
            totalMarkerCount += markerCount;
        }

        kmer.write(csv, k);
        csv << "," << totalMarkerCount << ",";
        for(uint64_t coverage=1; coverage<hMaxSize; coverage++) {
            uint64_t vertexCount = 0;
            if(coverage < h.size()) {
                vertexCount = h[coverage];
            }
            const uint64_t markerCount = coverage * vertexCount;
            csv << markerCount << ",";
        }
        csv << "\n";
    }
}



#ifdef SHASTA_HTTP_SERVER
bool Assembler::extractLocalMarkerGraph(
    OrientedReadId orientedReadId,
    uint32_t ordinal,
    uint64_t distance,
    int timeout,                 // Or 0 for no timeout.
    uint64_t minVertexCoverage,
    uint64_t minEdgeCoverage,
    bool useWeakEdges,
    bool usePrunedEdges,
    bool useSuperBubbleEdges,
    bool useLowCoverageCrossEdges,
    LocalMarkerGraph& graph
    )
{
    const MarkerGraph::VertexId startVertexId =
        getGlobalMarkerGraphVertex(orientedReadId, ordinal);
    return extractLocalMarkerGraph(
        startVertexId, distance, timeout,
        minVertexCoverage,
        minEdgeCoverage,
        useWeakEdges,
        usePrunedEdges,
        useSuperBubbleEdges,
        useLowCoverageCrossEdges,
        graph);

}



bool Assembler::extractLocalMarkerGraph(
    MarkerGraph::VertexId startVertexId,
    uint64_t distance,
    int timeout,                 // Or 0 for no timeout.
    uint64_t minVertexCoverage,
    uint64_t minEdgeCoverage,
    bool useWeakEdges,
    bool usePrunedEdges,
    bool useSuperBubbleEdges,
    bool useLowCoverageCrossEdges,
    LocalMarkerGraph& graph
    )
{
    // Sanity check.
    checkMarkerGraphEdgesIsOpen();

    // Some shorthands.
    AssemblyGraph& assemblyGraph = *assemblyGraphPointer;
    using vertex_descriptor = LocalMarkerGraph::vertex_descriptor;
    using edge_descriptor = LocalMarkerGraph::edge_descriptor;

    // Start a timer.
    const auto startTime = steady_clock::now();

    // Add the start vertex, without regards to coverage.
    if(startVertexId == MarkerGraph::invalidCompressedVertexId) {
        return true;    // Because no timeout occurred.
    }
    const vertex_descriptor vStart = graph.addVertex(startVertexId, 0, markerGraph.getVertexMarkerIds(startVertexId));

    // Some vectors used inside the BFS.
    // Define them here to reduce memory allocation activity.
    vector<MarkerInterval> markerIntervals;


    // Do the BFS to generate the vertices.
    // Edges will be created later.
    std::queue<vertex_descriptor> q;
    if(distance > 0) {
        q.push(vStart);
    }
    while(!q.empty()) {

        // See if we exceeded the timeout.
        if(timeout>0. && seconds(steady_clock::now() - startTime) > timeout) {
            graph.clear();
            return false;
        }

        // Dequeue a vertex.
        const vertex_descriptor v0 = q.front();
        q.pop();
        const LocalMarkerGraphVertex& vertex0 = graph[v0];
        const MarkerGraph::VertexId vertexId0 = vertex0.vertexId;
        const uint64_t distance0 = vertex0.distance;
        const uint64_t distance1 = distance0 + 1;

        // Loop over the children.
        const auto childEdges = markerGraph.edgesBySource[vertexId0];
        for(uint64_t edgeId: childEdges) {
            const auto& edge = markerGraph.edges[edgeId];

            // Skip this edge if the arguments require it.
            if(markerGraph.edgeMarkerIntervals[edgeId].size() < minEdgeCoverage) {
                continue;
            }
            if(edge.wasRemovedByTransitiveReduction && !useWeakEdges) {
                continue;
            }
            if(edge.wasPruned && !usePrunedEdges) {
                continue;
            }
            if(edge.isSuperBubbleEdge && !useSuperBubbleEdges) {
                continue;
            }
            if(edge.isLowCoverageCrossEdge && !useLowCoverageCrossEdges) {
                continue;
            }

            const MarkerGraph::VertexId vertexId1 = edge.target;
            SHASTA_ASSERT(edge.source == vertexId0);
            SHASTA_ASSERT(vertexId1 < markerGraph.vertexCount());

            // If vertex coverage is too low, skip it.
            if(markerGraph.vertexCoverage(vertexId1) < minVertexCoverage) {
                continue;
            }

            // Find the vertex corresponding to this child, creating it if necessary.
            bool vertexExists;
            vertex_descriptor v1;
            tie(vertexExists, v1) = graph.findVertex(vertexId1);
            if(!vertexExists) {
                v1 = graph.addVertex(
                    vertexId1, distance1, markerGraph.getVertexMarkerIds(vertexId1));
                if(distance1 < distance) {
                    q.push(v1);
                }
            }
        }

        // Loop over the parents.
        const auto parentEdges = markerGraph.edgesByTarget[vertexId0];
        for(uint64_t edgeId: parentEdges) {
            const auto& edge = markerGraph.edges[edgeId];

            // Skip this edge if the arguments require it.
            if(markerGraph.edgeMarkerIntervals[edgeId].size() < minEdgeCoverage) {
                continue;
            }
            if(edge.wasRemovedByTransitiveReduction && !useWeakEdges) {
                continue;
            }
            if(edge.wasPruned && !usePrunedEdges) {
                continue;
            }
            if(edge.isSuperBubbleEdge && !useSuperBubbleEdges) {
                continue;
            }
            if(edge.isLowCoverageCrossEdge && !useLowCoverageCrossEdges) {
                continue;
            }

            const MarkerGraph::VertexId vertexId1 = edge.source;
            SHASTA_ASSERT(edge.target == vertexId0);
            SHASTA_ASSERT(vertexId1 < markerGraph.vertexCount());

            // If vertex coverage is too low, skip it.
            if(markerGraph.vertexCoverage(vertexId1) < minVertexCoverage) {
                continue;
            }

            // Find the vertex corresponding to this child, creating it if necessary.
            bool vertexExists;
            vertex_descriptor v1;
            tie(vertexExists, v1) = graph.findVertex(vertexId1);
            if(!vertexExists) {
                v1 = graph.addVertex(
                    vertexId1, distance1, markerGraph.getVertexMarkerIds(vertexId1));
                if(distance1 < distance) {
                    q.push(v1);
                }
            }
        }
    }



    // Create edges.
    BGL_FORALL_VERTICES(v0, graph, LocalMarkerGraph) {
        const LocalMarkerGraphVertex& vertex0 = graph[v0];
        const MarkerGraph::VertexId vertexId0 = vertex0.vertexId;

        // Loop over the children that exist in the local marker graph
        // and are also at maximum distance.
        const auto childEdges = markerGraph.edgesBySource[vertexId0];
        for(uint64_t edgeId: childEdges) {
            const auto& edge = markerGraph.edges[edgeId];

            // Skip this edge if the arguments require it.
            if(markerGraph.edgeMarkerIntervals[edgeId].size() < minEdgeCoverage) {
                continue;
            }
            if(edge.wasRemovedByTransitiveReduction && !useWeakEdges) {
                continue;
            }
            if(edge.wasPruned && !usePrunedEdges) {
                continue;
            }
            if(edge.isSuperBubbleEdge && !useSuperBubbleEdges) {
                continue;
            }
            if(edge.isLowCoverageCrossEdge && !useLowCoverageCrossEdges) {
                continue;
            }

            const MarkerGraph::VertexId vertexId1 = edge.target;
            SHASTA_ASSERT(edge.source == vertexId0);
            SHASTA_ASSERT(vertexId1 < markerGraph.vertexCount());

            // See if we have a vertex for this global vertex id.
            bool vertexExists;
            vertex_descriptor v1;
            tie(vertexExists, v1) = graph.findVertex(vertexId1);

            // If it does not exist in the local marker graph, skip.
            if(!vertexExists) {
                continue;
            }

            // Add the edge.
            edge_descriptor e;
            bool edgeWasAdded = false;
            tie(e, edgeWasAdded) = boost::add_edge(v0, v1, graph);
            SHASTA_ASSERT(edgeWasAdded);

            // Fill in edge information.
            const auto storedMarkerIntervals = markerGraph.edgeMarkerIntervals[edgeId];
            markerIntervals.resize(storedMarkerIntervals.size());
            copy(storedMarkerIntervals.begin(), storedMarkerIntervals.end(), markerIntervals.begin());
            graph.storeEdgeInfo(e, markerIntervals);
            graph[e].edgeId = edgeId;
            graph[e].wasRemovedByTransitiveReduction = markerGraph.edges[edgeId].wasRemovedByTransitiveReduction;
            graph[e].wasPruned = markerGraph.edges[edgeId].wasPruned;
            graph[e].isSuperBubbleEdge = markerGraph.edges[edgeId].isSuperBubbleEdge;
            graph[e].isLowCoverageCrossEdge = markerGraph.edges[edgeId].isLowCoverageCrossEdge;
            graph[e].wasAssembled = markerGraph.edges[edgeId].wasAssembled;
            graph[e].isSecondary = markerGraph.edges[edgeId].isSecondary;

            // Link to assembly graph vertex.
            if(assemblyGraph.markerToAssemblyTable.isOpen()) {
                const auto& locations = assemblyGraph.markerToAssemblyTable[edgeId];
                copy(locations.begin(), locations.end(),
                    back_inserter(graph[e].assemblyGraphLocations));
            }
        }
    }

    // Store consensus repeat counts for all vertices.
    if(markerGraph.vertexRepeatCounts.isOpen) {
        const size_t k = assemblerInfo->k;
        BGL_FORALL_VERTICES(v, graph, LocalMarkerGraph) {
            LocalMarkerGraphVertex& vertex = graph[v];
            vertex.storedConsensusRepeatCounts.resize(k);
            const uint8_t* begin = markerGraph.vertexRepeatCounts.begin() + k * vertex.vertexId;
            copy(begin, begin+k, vertex.storedConsensusRepeatCounts.begin());
        }
    }

    // For better display with dot layout, do
    // an approximate topological sort.
    // Back-edges are more likely to be low coverage edges.
    graph.approximateTopologicalSort();

    // Fill in the ConsensusInfo's for each vertex.
    graph.computeVertexConsensusInfo();

    // Fill in the consensus sequence for all edges.
    const uint32_t markerGraphEdgeLengthThresholdForConsensus = 1000;

    const spoa::AlignmentType alignmentType = spoa::AlignmentType::kNW;
    const int8_t match = 1;
    const int8_t mismatch = -1;
    const int8_t gap = -1;
    auto spoaAlignmentEngine = spoa::createAlignmentEngine(alignmentType, match, mismatch, gap);
    auto spoaAlignmentGraph = spoa::createGraph();
    BGL_FORALL_EDGES(e, graph, LocalMarkerGraph) {
        LocalMarkerGraphEdge& edge = graph[e];
        ComputeMarkerGraphEdgeConsensusSequenceUsingSpoaDetail detail;
        computeMarkerGraphEdgeConsensusSequenceUsingSpoa(
            edge.edgeId,
            markerGraphEdgeLengthThresholdForConsensus,
            spoaAlignmentEngine,
            spoaAlignmentGraph,
            edge.consensusSequence,
            edge.consensusRepeatCounts,
            edge.consensusOverlappingBaseCount,
            detail,
            0);
    }

    return true;
}
#endif



// Compute edges of the global marker graph.
void Assembler::createMarkerGraphEdges(size_t threadCount)
{
    cout << timestamp << "createMarkerGraphEdges begins." << endl;

    // Check that we have what we need.
    checkMarkerGraphVerticesAreAvailable();

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Each thread stores the edges it finds in a separate vector.
    createMarkerGraphEdgesData.threadEdges.resize(threadCount);
    createMarkerGraphEdgesData.threadEdgeMarkerIntervals.resize(threadCount);
    cout << timestamp << "Processing " << markerGraph.vertexCount();
    cout << " marker graph vertices." << endl;
    setupLoadBalancing(markerGraph.vertexCount(), 100);
    runThreads(&Assembler::createMarkerGraphEdgesThreadFunction0, threadCount);

    // Combine the edges found by each thread.
    cout << timestamp << "Combining the edges found by each thread." << endl;
    markerGraph.edges.createNew(
            largeDataName("GlobalMarkerGraphEdges"),
            largeDataPageSize);
    markerGraph.edgeMarkerIntervals.createNew(
            largeDataName("GlobalMarkerGraphEdgeMarkerIntervals"),
            largeDataPageSize);
    for(size_t threadId=0; threadId<threadCount; threadId++) {
        auto& thisThreadEdges = *createMarkerGraphEdgesData.threadEdges[threadId];
        auto& thisThreadEdgeMarkerIntervals = *createMarkerGraphEdgesData.threadEdgeMarkerIntervals[threadId];
        SHASTA_ASSERT(thisThreadEdges.size() == thisThreadEdgeMarkerIntervals.size());
        for(size_t i=0; i<thisThreadEdges.size(); i++) {
            const auto& edge = thisThreadEdges[i];
            const auto edgeMarkerIntervals = thisThreadEdgeMarkerIntervals[i];
            markerGraph.edges.push_back(edge);
            markerGraph.edgeMarkerIntervals.appendVector();
            for(auto edgeMarkerInterval: edgeMarkerIntervals) {
                markerGraph.edgeMarkerIntervals.append(edgeMarkerInterval);
            }
        }
        thisThreadEdges.remove();
        thisThreadEdgeMarkerIntervals.remove();
    }

    markerGraph.edges.unreserve();
    markerGraph.edgeMarkerIntervals.unreserve();
    
    SHASTA_ASSERT(markerGraph.edges.size() == markerGraph.edgeMarkerIntervals.size());
    cout << timestamp << "Found " << markerGraph.edges.size();
    cout << " edges for " << markerGraph.vertexCount() << " vertices." << endl;



    // Now we need to create edgesBySource and edgesByTarget.
    createMarkerGraphEdgesBySourceAndTarget(threadCount);
    cout << timestamp << "createMarkerGraphEdges ends." << endl;
}



void Assembler::createMarkerGraphEdgesBySourceAndTarget(size_t threadCount)
{
    markerGraph.edgesBySource.createNew(
        largeDataName("GlobalMarkerGraphEdgesBySource"),
        largeDataPageSize);
    markerGraph.edgesByTarget.createNew(
        largeDataName("GlobalMarkerGraphEdgesByTarget"),
        largeDataPageSize);

    // cout << timestamp << "Create marker graph edges by source and target: pass 1 begins." << endl;
    markerGraph.edgesBySource.beginPass1(markerGraph.vertexCount());
    markerGraph.edgesByTarget.beginPass1(markerGraph.vertexCount());
    setupLoadBalancing(markerGraph.edges.size(), 100000);
    runThreads(&Assembler::createMarkerGraphEdgesThreadFunction1, threadCount);

    // cout << timestamp << "Create marker graph edges by source and target: pass 2 begins." << endl;
    markerGraph.edgesBySource.beginPass2();
    markerGraph.edgesByTarget.beginPass2();
    setupLoadBalancing(markerGraph.edges.size(), 100000);
    runThreads(&Assembler::createMarkerGraphEdgesThreadFunction2, threadCount);
    markerGraph.edgesBySource.endPass2();
    markerGraph.edgesByTarget.endPass2();

}



void Assembler::createMarkerGraphEdgesThreadFunction0(size_t threadId)
{
    using std::shared_ptr;
    using std::make_shared;

    // Create the vector to contain the edges found by this thread.
    shared_ptr< MemoryMapped::Vector<MarkerGraph::Edge> > thisThreadEdgesPointer =
        make_shared< MemoryMapped::Vector<MarkerGraph::Edge> >();
    createMarkerGraphEdgesData.threadEdges[threadId] = thisThreadEdgesPointer;
    MemoryMapped::Vector<MarkerGraph::Edge>& thisThreadEdges = *thisThreadEdgesPointer;
    thisThreadEdges.createNew(
            largeDataName("tmp-ThreadGlobalMarkerGraphEdges-" + to_string(threadId)),
            largeDataPageSize);

    // Create the vector to contain the marker intervals for edges found by this thread.
    shared_ptr< MemoryMapped::VectorOfVectors<MarkerInterval, uint64_t> >
        thisThreadEdgeMarkerIntervalsPointer =
        make_shared< MemoryMapped::VectorOfVectors<MarkerInterval, uint64_t> >();
    createMarkerGraphEdgesData.threadEdgeMarkerIntervals[threadId] = thisThreadEdgeMarkerIntervalsPointer;
    MemoryMapped::VectorOfVectors<MarkerInterval, uint64_t>&
        thisThreadEdgeMarkerIntervals = *thisThreadEdgeMarkerIntervalsPointer;
    thisThreadEdgeMarkerIntervals.createNew(
            largeDataName("tmp-ThreadGlobalMarkerGraphEdgeMarkerIntervals-" + to_string(threadId)),
            largeDataPageSize);

    // Some things used inside the loop but defined here for performance.
    vector< pair<MarkerGraph::VertexId, vector<MarkerInterval> > > children;
    vector< pair<MarkerGraph::VertexId, MarkerInterval> > workArea;
    MarkerGraph::Edge edge;

    // Loop over all batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // Loop over all marker graph vertices assigned to this batch.
        for(MarkerGraph::VertexId vertex0=begin; vertex0!=end; ++vertex0) {
            edge.source = vertex0;

            getGlobalMarkerGraphVertexChildren(vertex0, children, workArea);
            for(const auto& p: children) {
                const auto vertex1 = p.first;
                const auto& markerIntervals = p.second;
                edge.target = vertex1;
                size_t coverage = markerIntervals.size();
                if(coverage < 256) {
                    edge.coverage = uint8_t(coverage);
                } else {
                    edge.coverage = 255;
                }

                // Store the edge.
                thisThreadEdges.push_back(edge);

                // Store the marker intervals.
                thisThreadEdgeMarkerIntervals.appendVector();
                for(const MarkerInterval markerInterval: markerIntervals) {
                    thisThreadEdgeMarkerIntervals.append(markerInterval);
                }
            }
        }
    }

    thisThreadEdges.unreserve();
    thisThreadEdgeMarkerIntervals.unreserve();
}



void Assembler::createMarkerGraphEdgesThreadFunction1(size_t threadId)
{
    createMarkerGraphEdgesThreadFunction12(threadId, 1);
}
void Assembler::createMarkerGraphEdgesThreadFunction2(size_t threadId)
{
    createMarkerGraphEdgesThreadFunction12(threadId, 2);
}
void Assembler::createMarkerGraphEdgesThreadFunction12(size_t threadId, size_t pass)
{
    SHASTA_ASSERT(pass==1 || pass==2);

    // Loop over all batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // Loop over all marker graph edges assigned to this batch.
        for(uint64_t i=begin; i!=end; ++i) {
            const auto& edge = markerGraph.edges[i];
            if(pass == 1) {
                markerGraph.edgesBySource.incrementCountMultithreaded(edge.source);
                markerGraph.edgesByTarget.incrementCountMultithreaded(edge.target);
            } else {
                markerGraph.edgesBySource.storeMultithreaded(edge.source, Uint40(i));
                markerGraph.edgesByTarget.storeMultithreaded(edge.target, Uint40(i));
            }
        }
    }

}


void Assembler::accessMarkerGraphEdges(
    bool accessEdgesReadWrite,
    bool accessConnectivityReadWrite)
{
    if(accessEdgesReadWrite) {
        markerGraph.edges.accessExistingReadWrite(
            largeDataName("GlobalMarkerGraphEdges"));
        markerGraph.edgeMarkerIntervals.accessExistingReadWrite(
            largeDataName("GlobalMarkerGraphEdgeMarkerIntervals"));
    } else {
        markerGraph.edges.accessExistingReadOnly(
            largeDataName("GlobalMarkerGraphEdges"));
        markerGraph.edgeMarkerIntervals.accessExistingReadOnly(
            largeDataName("GlobalMarkerGraphEdgeMarkerIntervals"));
    }

    if(accessConnectivityReadWrite) {
        markerGraph.edgesBySource.accessExistingReadWrite(
            largeDataName("GlobalMarkerGraphEdgesBySource"));
        markerGraph.edgesByTarget.accessExistingReadWrite(
            largeDataName("GlobalMarkerGraphEdgesByTarget"));
    } else {
        markerGraph.edgesBySource.accessExistingReadOnly(
            largeDataName("GlobalMarkerGraphEdgesBySource"));
        markerGraph.edgesByTarget.accessExistingReadOnly(
            largeDataName("GlobalMarkerGraphEdgesByTarget"));
    }
}



void Assembler::checkMarkerGraphEdgesIsOpen() const
{
    SHASTA_ASSERT(markerGraph.edges.isOpen);
    SHASTA_ASSERT(markerGraph.edgesBySource.isOpen());
    SHASTA_ASSERT(markerGraph.edgesByTarget.isOpen());
}



// Approximate transitive reduction of the marker graph.
// This does the following, in this order:
// - All edges with coverage less than or equal to lowCoverageThreshold
//   are marked wasRemovedByTransitiveReduction.
// - All edges with coverage 1 and a marker skip
//   greater than edgeMarkerSkipThreshold
//   are marked wasRemovedByTransitiveReduction.
// - Edges with coverage greater than lowCoverageThreshold
//   and less then highCoverageThreshold are processed in
//   ordered of increasing coverage:
//   * For each such edge A->B, we look for a path of length
//     at most maxDistance between A and B that does not use
//     edge A->B and also does not use any
//     edges already marked wasRemovedByTransitiveReduction.
//   * If such a path is found, the edge is marked
//     wasRemovedByTransitiveReduction.
// - Edges with coverage highCoverageThreshold or greater
//   are left untouched.
// The marker graph is guaranteed to be strand symmetric
// when this begins, and we have to guarantee that it remains
// strand symmetric when this ends.
// To achieve this, we always process the two edges
// in a reverse complemented pair together.
void Assembler::transitiveReduction(
    size_t lowCoverageThreshold,
    size_t highCoverageThreshold,
    size_t maxDistance,
    size_t edgeMarkerSkipThreshold)
{
    // Some shorthands for readability.
    auto& edges = markerGraph.edges;
    using VertexId = MarkerGraph::VertexId;
    using EdgeId = MarkerGraph::EdgeId;
    using Edge = MarkerGraph::Edge;

    // Initial message.
    cout << timestamp << "Transitive reduction of the marker graph begins." << endl;
    cout << "The marker graph has " << markerGraph.vertexCount() << " vertices and ";
    cout << edges.size() << " edges." << endl;

    // Initially flag all edges as not removed by transitive reduction.
    // To facilitate debugging, also clear the other flags, which are
    // set later in the normal assembly process.
    for(auto& edge: edges) {
        edge.wasRemovedByTransitiveReduction = 0;
        edge.wasPruned = 0;
        edge.isSuperBubbleEdge = 0;
    }

    // Gather edges for each coverage less than highCoverageThreshold.
    // Only add to the list those with id less than the id of their reverse complement.
    MemoryMapped::VectorOfVectors<EdgeId, EdgeId>  edgesByCoverage;
    edgesByCoverage.createNew(
            largeDataName("tmp-flagMarkerGraphWeakEdges-edgesByCoverage"),
            largeDataPageSize);
    edgesByCoverage.beginPass1(highCoverageThreshold);
    for(EdgeId edgeId=0; edgeId!=edges.size(); edgeId++) {
        if (markerGraph.reverseComplementEdge[edgeId] < edgeId) {
            continue;
        }
        const MarkerGraph::Edge& edge = edges[edgeId];
        if(edge.coverage < highCoverageThreshold) {
            edgesByCoverage.incrementCount(edge.coverage);
        }
    }
    edgesByCoverage.beginPass2();
    for(EdgeId edgeId=0; edgeId!=edges.size(); edgeId++) {
        if (markerGraph.reverseComplementEdge[edgeId] < edgeId) {
            continue;
        }
        const MarkerGraph::Edge& edge = edges[edgeId];
        if(edge.coverage < highCoverageThreshold) {
            edgesByCoverage.store(edge.coverage, edgeId);
        }
    }
    edgesByCoverage.endPass2();

    // Check that there are no edges with coverage 0.
    SHASTA_ASSERT(edgesByCoverage[0].size() == 0);

    // Vector to contain vertex distances during each BFS.
    // Is is set to -1 for vertices not reached by the BFS.
    MemoryMapped::Vector<int> vertexDistances;
    vertexDistances.createNew(
        largeDataName("tmp-flagMarkerGraphWeakEdges-vertexDistances"),
        largeDataPageSize);
    vertexDistances.resize(markerGraph.vertexCount());
    fill(vertexDistances.begin(), vertexDistances.end(), -1);

    // Queue to be used for all BFSs.
    std::queue<VertexId> q;

    // Vector to store vertices encountered during a BFS.
    vector<VertexId> bfsVertices;



    // Flag as weak all edges with coverage <= lowCoverageThreshold
    for(size_t coverage=1; coverage<=lowCoverageThreshold; coverage++) {
        const auto& edgesWithThisCoverage = edgesByCoverage[coverage];
        if(edgesWithThisCoverage.size() > 0) {
            cout << timestamp << "Flagging as weak " << 2 * edgesWithThisCoverage.size() << " edges with coverage "
                << coverage << "." << endl;
        }
        for(const EdgeId edgeId: edgesWithThisCoverage) {
            edges[edgeId].wasRemovedByTransitiveReduction = 1;
            edges[markerGraph.reverseComplementEdge[edgeId]].wasRemovedByTransitiveReduction = 1;
        }
    }



    // Flag as weak all edges with coverage 1 and a marker skip
    // greater than edgeMarkerSkipThreshold
    const auto& edgesWithCoverage1 = edgesByCoverage[1];
    size_t coverage1HighSkipCount = 0;
    for(const EdgeId edgeId: edgesWithCoverage1) {
        const span<MarkerInterval> markerIntervals =
            markerGraph.edgeMarkerIntervals[edgeId];
        if(markerIntervals.size() > 1) {
            continue;
        }
        const MarkerInterval& markerInterval = markerIntervals[0];
        const uint32_t skip = markerInterval.ordinals[1] - markerInterval.ordinals[0];
        if(skip > edgeMarkerSkipThreshold) {
            if(edges[edgeId].wasRemovedByTransitiveReduction == 0) {
                edges[edgeId].wasRemovedByTransitiveReduction = 1;
                edges[markerGraph.reverseComplementEdge[edgeId]].wasRemovedByTransitiveReduction = 1;
                coverage1HighSkipCount += 2;
            }
        }
    }
    cout << timestamp << "Flagged as weak " << coverage1HighSkipCount <<
        " edges with coverage 1 and marker skip greater than " <<
        edgeMarkerSkipThreshold << endl;



    // Process edges of intermediate coverage.
    for(size_t coverage=lowCoverageThreshold+1;
        coverage<highCoverageThreshold; coverage++) {
        const auto& edgesWithThisCoverage = edgesByCoverage[coverage];
        if(edgesWithThisCoverage.size() == 0) {
            continue;
        }
        size_t count = 0;

        // Loop over edges with this coverage.
        for(const EdgeId edgeId: edgesWithThisCoverage) {
            const Edge& edge = edges[edgeId];
            if(edge.wasRemovedByTransitiveReduction) {
                continue;
            }
            const VertexId u0 = edge.source;
            const VertexId u1 = edge.target;

            // Do a forward BFS starting at u0, up to distance maxDistance,
            // using only edges currently marked as strong
            // and without using this edge.
            // If we encounter u1, u1 is reachable from v0 without
            // using this edge, and so we can mark this edge as weak.
            q.push(u0);
            vertexDistances[u0] = 0;
            bfsVertices.push_back(u0);
            bool found = false;
            while(!q.empty()) {
                const VertexId v0 = q.front();
                q.pop();
                const int distance0 = vertexDistances[v0];
                const int distance1 = distance0 + 1;
                for(const auto edgeId01: markerGraph.edgesBySource[v0]) {
                    if(edgeId01 == edgeId) {
                        continue;
                    }
                    const Edge& edge01 = markerGraph.edges[edgeId01];
                    if(edge01.wasRemovedByTransitiveReduction) {
                        continue;
                    }
                    const VertexId v1 = edge01.target;
                    if(vertexDistances[v1] >= 0) {
                        continue;   // We already encountered this vertex.
                    }
                    if(v1 == u1) {
                        // We found it!
                        found = true;
                        break;
                    }
                    vertexDistances[v1] = distance1;
                    bfsVertices.push_back(v1);
                    if(distance1 < int(maxDistance)) {
                        q.push(v1);
                    }
                }
                if(found) {
                    break;
                }
            }

            if(found) {
                edges[edgeId].wasRemovedByTransitiveReduction = 1;
                edges[markerGraph.reverseComplementEdge[edgeId]].wasRemovedByTransitiveReduction = 1;
                count += 2;
            }

            // Clean up to be ready to process the next edge.
            while(!q.empty()) {
                q.pop();
            }
            for(const VertexId v: bfsVertices) {
                vertexDistances[v] = -1;
            }
            bfsVertices.clear();
        }

        if(count) {
            cout << timestamp << "Flagged as weak " << count <<
                " edges with coverage " << coverage <<
                " out of "<< 2*edgesWithThisCoverage.size() << " total." << endl;
        }
    }


    // Clean up our work areas.
    edgesByCoverage.remove();
    // edgeFlags.remove();
    vertexDistances.remove();



    // Count the number of edges that were flagged as weak.
    uint64_t weakEdgeCount = 0;;
    for(const auto& edge: markerGraph.edges) {
        if(edge.wasRemovedByTransitiveReduction) {
            ++weakEdgeCount;
        }
    }
    cout << "Transitive reduction removed " << weakEdgeCount << " marker graph edges out of ";
    cout << markerGraph.edges.size() << " total." << endl;

    cout << "The marker graph has " << markerGraph.vertexCount() << " vertices and ";
    cout << markerGraph.edges.size()-weakEdgeCount << " strong edges." << endl;

    cout << timestamp << "Transitive reduction of the marker graph ends." << endl;
}



// Approximate reverse transitive reduction of the marker graph.
// The goal is to remove local back-edges.
// This works similarly to transitive reduction,
// but in the opposite direction.
// This does the following:
// - Edges with coverage greater than lowCoverageThreshold
//   and less then highCoverageThreshold are processed in
//   ordered of increasing coverage:
//   * For each such edge A->B, we look for a path of length
//     at most maxDistance starting at B and ending at A
//     that does not use edge A->B and also does not use any
//     edges already marked wasRemovedByTransitiveReduction.
//   * If such a path is found, the edge is marked
//     wasRemovedByTransitiveReduction.
void Assembler::reverseTransitiveReduction(
    size_t lowCoverageThreshold,
    size_t highCoverageThreshold,
    size_t maxDistance)
{
    // Some shorthands for readability.
    auto& edges = markerGraph.edges;
    using VertexId = MarkerGraph::VertexId;
    using EdgeId = MarkerGraph::EdgeId;
    using Edge = MarkerGraph::Edge;

    // Initial message.
    cout << timestamp << "Reverse transitive reduction of the marker graph begins." << endl;
    cout << "The marker graph has " << markerGraph.vertexCount() << " vertices and ";
    cout << edges.size() << " edges." << endl;

    // Gather edges for each coverage less than highCoverageThreshold.
    // Only add to the list those with id less than the id of their reverse complement.
    MemoryMapped::VectorOfVectors<EdgeId, EdgeId>  edgesByCoverage;
    edgesByCoverage.createNew(
            largeDataName("tmp-flagMarkerGraphWeakEdges-edgesByCoverage"),
            largeDataPageSize);
    edgesByCoverage.beginPass1(highCoverageThreshold);
    for(EdgeId edgeId=0; edgeId!=edges.size(); edgeId++) {
        if (markerGraph.reverseComplementEdge[edgeId] < edgeId) {
            continue;
        }
        const MarkerGraph::Edge& edge = edges[edgeId];
        if(edge.coverage>lowCoverageThreshold && edge.coverage<highCoverageThreshold) {
            edgesByCoverage.incrementCount(edge.coverage);
        }
    }
    edgesByCoverage.beginPass2();
    for(EdgeId edgeId=0; edgeId!=edges.size(); edgeId++) {
        if (markerGraph.reverseComplementEdge[edgeId] < edgeId) {
            continue;
        }
        const MarkerGraph::Edge& edge = edges[edgeId];
        if(edge.coverage>lowCoverageThreshold && edge.coverage<highCoverageThreshold) {
            edgesByCoverage.store(edge.coverage, edgeId);
        }
    }
    edgesByCoverage.endPass2();

    // Vector to contain vertex distances during each BFS.
    // Is is set to -1 for vertices not reached by the BFS.
    MemoryMapped::Vector<int> vertexDistances;
    vertexDistances.createNew(
        largeDataName("tmp-flagMarkerGraphWeakEdges-vertexDistances"),
        largeDataPageSize);
    vertexDistances.resize(markerGraph.vertexCount());
    fill(vertexDistances.begin(), vertexDistances.end(), -1);

    // Queue to be used for all BFSs.
    std::queue<VertexId> q;

    // Vector to store vertices encountered during a BFS.
    vector<VertexId> bfsVertices;



    // Process edges in the specified coverage range.
    size_t removedCount = 0;
    for(size_t coverage=lowCoverageThreshold+1;
        coverage<highCoverageThreshold; coverage++) {
        const auto& edgesWithThisCoverage = edgesByCoverage[coverage];
        if(edgesWithThisCoverage.size() == 0) {
            continue;
        }
        size_t count = 0;

        // Loop over edges with this coverage.
        for(const EdgeId edgeId: edgesWithThisCoverage) {
            const Edge& edge = edges[edgeId];
            if(edge.wasRemovedByTransitiveReduction) {
                continue;
            }
            const VertexId u0 = edge.target;
            const VertexId u1 = edge.source;

            // Do a forward BFS starting at u0, up to distance maxDistance,
            // using only edges currently marked as strong
            // and without using this edge.
            // If we encounter u1, u1 is reachable from u0 without
            // using this edge, and so we can mark this edge as weak.
            q.push(u0);
            vertexDistances[u0] = 0;
            bfsVertices.push_back(u0);
            bool found = false;
            while(!q.empty()) {
                const VertexId v0 = q.front();
                q.pop();
                const int distance0 = vertexDistances[v0];
                const int distance1 = distance0 + 1;
                for(const auto edgeId01: markerGraph.edgesBySource[v0]) {
                    if(edgeId01 == edgeId) {
                        continue;
                    }
                    const Edge& edge01 = markerGraph.edges[edgeId01];
                    if(edge01.wasRemovedByTransitiveReduction) {
                        continue;
                    }
                    const VertexId v1 = edge01.target;
                    if(vertexDistances[v1] >= 0) {
                        continue;   // We already encountered this vertex.
                    }
                    if(v1 == u1) {
                        // We found it!
                        found = true;
                        break;
                    }
                    vertexDistances[v1] = distance1;
                    bfsVertices.push_back(v1);
                    if(distance1 < int(maxDistance)) {
                        q.push(v1);
                    }
                }
                if(found) {
                    break;
                }
            }

            if(found) {
                edges[edgeId].wasRemovedByTransitiveReduction = 1;
                edges[markerGraph.reverseComplementEdge[edgeId]].wasRemovedByTransitiveReduction = 1;
                count += 2;
            }

            // Clean up to be ready to process the next edge.
            while(!q.empty()) {
                q.pop();
            }
            for(const VertexId v: bfsVertices) {
                vertexDistances[v] = -1;
            }
            bfsVertices.clear();
        }

        if(count) {
            cout << timestamp << "Reverse transitive reduction removed " << count <<
                " edges with coverage " << coverage <<
                " out of "<< 2*edgesWithThisCoverage.size() << " total." << endl;
        }
        removedCount += count;
    }
    cout << timestamp << "Reverse transitive reduction removed " << removedCount <<" edges." << endl;


    // Clean up our work areas.
    edgesByCoverage.remove();
    // edgeFlags.remove();
    vertexDistances.remove();

    cout << timestamp << "Reverse transitive reduction of the marker graph ends." << endl;

}



// Return true if an edge disconnects the local subgraph.
bool Assembler::markerGraphEdgeDisconnectsLocalStrongSubgraph(
    MarkerGraph::EdgeId startEdgeId,
    size_t maxDistance,

    // Each of these two must be sized maxDistance.
    array<vector< vector<MarkerGraph::EdgeId> >, 2>& verticesByDistance,

    // Each of these two must be sized markerGraph.vertexCount()
    // and set to all false on entry.
    // It is left set to all false on exit, so it can be reused.
    array<vector<bool>, 2>& vertexFlags
    ) const
{

    // Some shorthands for clarity.
    auto& edges = markerGraph.edges;
    using VertexId = MarkerGraph::VertexId;
    using EdgeId = MarkerGraph::EdgeId;
    using Edge = MarkerGraph::Edge;

    // Check that the work areas are sized as expected.
    for(size_t i=0; i<2; i++) {
        SHASTA_ASSERT(verticesByDistance[i].size() == maxDistance+1);
        SHASTA_ASSERT(vertexFlags[i].size() == markerGraph.vertexCount());
    }

    // Find the two vertices of the starting edge.
    const Edge& startEdge = edges[startEdgeId];
    const array<VertexId, 2> startVertexIds = {startEdge.source, startEdge.target};

    // We want to the two BFS one step at a time,
    // going up by one in distance at each step,
    // and avoiding the start edge.
    // So, instead of the usual queue, we store the vertices found at each distance
    // for each of the two start vertices.
    // verticesByDistance is indexed by [0 or 1][distance].
    for(size_t i=0; i<2; i++) {
        SHASTA_ASSERT(verticesByDistance[i][0].size() == 0);
        verticesByDistance[i][0].clear();
        const VertexId startVertexId = startVertexIds[i];
        verticesByDistance[i][0].push_back(startVertexId);
        vertexFlags[i][startVertexId] = true;
    }
    // cout << "Working on edge " << startVertexIds[0] << " " << startVertexIds[1] << endl;



    // Do the two BFSs in order of increasing distance
    // and avoiding the start edge.
    // At each step we process vertices at distance
    // and find vertices at distance+1.
    bool disconnects = true;
    for(size_t distance=0; distance<maxDistance; distance++) {
        // cout << "Working on distance " << distance << endl;
        for(size_t i=0; i<2; i++) {
            // cout << "Working on i = " << i << endl;
            SHASTA_ASSERT(verticesByDistance[i][distance+1].size() == 0);
            for(const VertexId vertexId0: verticesByDistance[i][distance]) {
                // cout << "VertexId0 " << vertexId0 << endl;

                // Loop over children.
                auto childEdgeIds = markerGraph.edgesBySource[vertexId0];
                for(EdgeId edgeId: childEdgeIds) {
                    if(edgeId == startEdgeId) {
                        continue;
                    }
                    const Edge& edge = edges[edgeId];
                    if(edge.wasRemovedByTransitiveReduction) {
                        continue;
                    }
                    const VertexId vertexId1 = edge.target;
                    // cout << "Distance " << distance << " i " << i << " found child " << vertexId1 << endl;

                    // If we already found this vertex in this BFS, skip it.
                    if(vertexFlags[i][vertexId1]) {
                        // cout << "Already found." << endl;
                        continue;
                    }

                    // If we already found this vertex in the other BFS,
                    // we have found a path between the vertices of the starting edge
                    // that does not use the starting edge.
                    // Therefore, the starting edge does not disconnect the local subgraph
                    if(vertexFlags[1-i][vertexId1]) {
                        // cout << "Already found on other BFS" << endl;
                        disconnects = false;
                        break;
                    }

                    // This is the first time we find this vertex.
                    verticesByDistance[i][distance+1].push_back(vertexId1);
                    vertexFlags[i][vertexId1] = true;
                }
                if(!disconnects) {
                    break;
                }

                // Loop over parents.
                auto parentEdgeIds = markerGraph.edgesByTarget[vertexId0];
                for(EdgeId edgeId: parentEdgeIds) {
                    if(edgeId == startEdgeId) {
                        continue;
                    }
                    const Edge& edge = edges[edgeId];
                    if(edge.wasRemovedByTransitiveReduction) {
                        continue;
                    }
                    const VertexId vertexId1 = edge.source;
                    // cout << "Distance " << distance << " i " << i << " found parent " << vertexId1 << endl;

                    // If we already found this vertex in this BFS, skip it.
                    if(vertexFlags[i][vertexId1]) {
                        // cout << "Already found." << endl;
                        continue;
                    }

                    // If we already found this vertex in the other BFS,
                    // we have found a path between the vertices of the starting edge
                    // that does not use the starting edge.
                    // Therefore, the starting edge does not disconnect the local subgraph
                    if(vertexFlags[1-i][vertexId1]) {
                        disconnects = false;
                        // cout << "Already found on other BFS" << endl;
                        break;
                    }

                    // This is the first time we find this vertex.
                    verticesByDistance[i][distance+1].push_back(vertexId1);
                    vertexFlags[i][vertexId1] = true;
                }
                if(!disconnects) {
                    break;
                }

            }
            if(!disconnects) {
                break;
            }
        }
        if(!disconnects) {
            break;
        }
    }



    // Clean up.
    for(size_t distance=0; distance<=maxDistance; distance++) {
        for(size_t i=0; i<2; i++) {
            auto& v = verticesByDistance[i][distance];
            for(const VertexId vertexId: v) {
                vertexFlags[i][vertexId] = false;
            }
            v.clear();
        }
    }


    // cout << "Returning " << int(disconnects) << endl;
    // SHASTA_ASSERT(0);
    return disconnects;
}



// Prune leaves from the strong subgraph of the global marker graph.
void Assembler::pruneMarkerGraphStrongSubgraph(size_t iterationCount)
{
    // Some shorthands.
    using VertexId = MarkerGraph::VertexId;
    using EdgeId = VertexId;

    // Check that we have what we need.
    checkMarkerGraphVerticesAreAvailable();
    checkMarkerGraphEdgesIsOpen();

    // Get the number of edges.
    auto& edges = markerGraph.edges;
    const EdgeId edgeCount = edges.size();

    // Flags to mark edges to prune at each iteration.
    MemoryMapped::Vector<bool> edgesToBePruned;
    edgesToBePruned.createNew(
        largeDataName("tmp-PruneMarkerGraphStrogngSubgraph"),
        largeDataPageSize);
    edgesToBePruned.resize(edgeCount);
    fill(edgesToBePruned.begin(), edgesToBePruned.end(), false);

    // Clear the wasPruned flag of all edges.
    for(MarkerGraph::Edge& edge: edges) {
        edge.wasPruned = 0;
    }



    // At each prune iteration we prune one layer of leaves.
    for(size_t iteration=0; iteration!=iterationCount; iteration++) {
        cout << timestamp << "Begin prune iteration " << iteration << endl;

        // Find the edges to be pruned at each iteration.
        for(EdgeId edgeId=0; edgeId<edgeCount; edgeId++) {
            MarkerGraph::Edge& edge = edges[edgeId];
            if(edge.wasRemovedByTransitiveReduction) {
                continue;
            }
            if(edge.wasPruned) {
                continue;
            }
            if(
                isForwardLeafOfMarkerGraphPrunedStrongSubgraph(edge.target) ||
                isBackwardLeafOfMarkerGraphPrunedStrongSubgraph(edge.source)
                ) {
                edgesToBePruned[edgeId] = true;
            }
        }



        // Flag the edges we found at this iteration.
        EdgeId count = 0;
        for(EdgeId edgeId=0; edgeId<edgeCount; edgeId++) {
            if(edgesToBePruned[edgeId]) {
                edges[edgeId].wasPruned = 1;
                ++count;
                edgesToBePruned[edgeId] = false;    // For next iteration.
            }
        }
        cout << "Pruned " << count << " edges at prune iteration " << iteration << "." << endl;
    }


    edgesToBePruned.remove();


    // Count the number of surviving edges in the pruned strong subgraph.
    size_t count = 0;
    for(MarkerGraph::Edge& edge: edges) {
        if(!edge.wasRemovedByTransitiveReduction && !edge.wasPruned) {
            ++count;
        }
    }
    cout << "The original marker graph had " << markerGraph.vertexCount();
    cout << " vertices and " << edgeCount << " edges." << endl;
    cout << "The number of surviving edges is " << count << "." << endl;
}


// Find out if a vertex is a forward or backward leaf of the pruned
// strong subgraph of the marker graph.
// A forward leaf is a vertex with out-degree 0.
// A backward leaf is a vertex with in-degree 0.
bool Assembler::isForwardLeafOfMarkerGraphPrunedStrongSubgraph(MarkerGraph::VertexId vertexId) const
{
    const auto& forwardEdges = markerGraph.edgesBySource[vertexId];
    for(const auto& edgeId: forwardEdges) {
        const auto& edge = markerGraph.edges[edgeId];
        if(!edge.wasRemovedByTransitiveReduction && !edge.wasPruned) {
            return false;   // We found a forward edge, so this is not a forward leaf.
        }

    }
    return true;    // We did not find any forward edges, so this is a forward leaf.
}
bool Assembler::isBackwardLeafOfMarkerGraphPrunedStrongSubgraph(MarkerGraph::VertexId vertexId) const
{
    const auto& backwardEdges = markerGraph.edgesByTarget[vertexId];
    for(const auto& edgeId: backwardEdges) {
        const auto& edge = markerGraph.edges[edgeId];
        if(!edge.wasRemovedByTransitiveReduction && !edge.wasPruned) {
            return false;   // We found a backward edge, so this is not a backward leaf.
        }

    }
    return true;    // We did not find any backward edges, so this is a backward leaf.
}



// Given an edge of the pruned strong subgraph of the marker graph,
// return the next edge in the linear chain the edge belongs to.
// If the edge is the last edge in its linear chain, return MarkerGraph::invalidEdgeId.
MarkerGraph::EdgeId Assembler::nextEdgeInMarkerGraphPrunedStrongSubgraphChain(
    MarkerGraph::EdgeId edgeId0) const
{
    // Some shorthands.
    using EdgeId = MarkerGraph::EdgeId;
    using Edge = MarkerGraph::Edge;
    const auto& edges = markerGraph.edges;

    // Check that the edge we were passed belongs to the
    // pruned spanning subgraph of the marker graph.
    const Edge& edge0 = edges[edgeId0];
    SHASTA_ASSERT(!edge0.wasRemoved());

    // If the out-degree and in-degree of the target of this edge are not both 1,
    // this edge is the last of its chain.
    if(
        (markerGraphPrunedStrongSubgraphOutDegree(edge0.target) != 1) ||
        (markerGraphPrunedStrongSubgraphInDegree( edge0.target) != 1)
        ) {
        return MarkerGraph::invalidEdgeId;
    }

    // Loop over all edges following it.
    EdgeId nextEdgeId = MarkerGraph::invalidEdgeId;
    for(const EdgeId edgeId1: markerGraph.edgesBySource[edge0.target]) {
        const Edge& edge1 = edges[edgeId1];

        // Skip the edge if it is not part of the
        // pruned strong subgraph of the marker graph.
        if(edge1.wasRemoved()) {
            continue;
        }

        // Ok, this a possible next edge.
        if(nextEdgeId == MarkerGraph::invalidEdgeId) {
            // This is the first one we find.
            nextEdgeId = edgeId1;
        } else {
            // This is not the first one we found, so the next edge is not unique.
            return MarkerGraph::invalidEdgeId;
        }
    }

    return nextEdgeId;
}



// Given an edge of the pruned strong subgraph of the marker graph,
// return the previous edge in the linear chain the edge belongs to.
// If the edge is the first edge in its linear chain, return MarkerGraph::invalidEdgeId.
MarkerGraph::EdgeId Assembler::previousEdgeInMarkerGraphPrunedStrongSubgraphChain(
    MarkerGraph::EdgeId edgeId0) const
{
    const bool debug = false;
    if(debug) {
        cout << "previousEdgeInMarkerGraphPrunedStrongSubgraphChain begins." << endl;
    }

    // Some shorthands.
    using EdgeId = MarkerGraph::EdgeId;
    using Edge = MarkerGraph::Edge;
    const auto& edges = markerGraph.edges;

    // Check that the edge we were passed belongs to the
    // pruned spanning subgraph of the marker graph.
    const Edge& edge0 = edges[edgeId0];
    SHASTA_ASSERT(!edge0.wasRemoved());

    // If the out-degree and in-degree of the source of this edge are not both 1,
    // this edge is the last of its chain.
    if(
        (markerGraphPrunedStrongSubgraphOutDegree(edge0.source) != 1) ||
        (markerGraphPrunedStrongSubgraphInDegree( edge0.source) != 1)
        ) {
        return MarkerGraph::invalidEdgeId;
    }

    // Loop over all edges preceding it.
    EdgeId previousEdgeId = MarkerGraph::invalidEdgeId;
    for(const EdgeId edgeId1: markerGraph.edgesByTarget[edge0.source]) {
        const Edge& edge1 = edges[edgeId1];
        if(debug) {
            cout << "Found " << edgeId1 << " " << edge1.source << "->" << edge1.target << endl;
        }

        // Skip the edge if it is not part of the
        // pruned strong subgraph of the marker graph.
        if(edge1.wasRemoved()) {
            if(debug) {
                cout << "Edge was removed." << endl;
            }
            continue;
        }

        // Ok, this a possible previous edge.
        if(previousEdgeId == MarkerGraph::invalidEdgeId) {
            // This is the first one we find.
            if(debug) {
                cout << "Tentative previous edge " << edgeId1 << " " << edge1.source << "->" << edge1.target << endl;
            }
            previousEdgeId = edgeId1;
        } else {
            // This is not the first one we found, so the previous edge is not unique.
            if(debug) {
                cout << "previousEdgeInMarkerGraphPrunedStrongSubgraphChain ends, case 1." << endl;
            }
            return MarkerGraph::invalidEdgeId;
        }
    }
    if(debug) {
        cout << "previousEdgeInMarkerGraphPrunedStrongSubgraphChain ends, case 2 " << previousEdgeId << endl;
    }

    return previousEdgeId;
}



// Return the out-degree or in-degree (number of outgoing/incoming edges)
// of a vertex of the pruned spanning subgraph of the marker graph.
size_t Assembler::markerGraphPrunedStrongSubgraphOutDegree(
    MarkerGraph::VertexId vertexId) const
{
    size_t outDegree = 0;
    for(const auto edgeId: markerGraph.edgesBySource[vertexId]) {
        const auto& edge = markerGraph.edges[edgeId];
        if(!edge.wasRemoved()) {
            ++outDegree;
        }
    }
    return outDegree;
}
size_t Assembler::markerGraphPrunedStrongSubgraphInDegree(
    MarkerGraph::VertexId vertexId) const
{
    size_t inDegree = 0;
    for(const auto edgeId: markerGraph.edgesByTarget[vertexId]) {
        const auto& edge = markerGraph.edges[edgeId];
        if(!edge.wasRemoved()) {
            ++inDegree;
        }
    }
    return inDegree;
}



// Compute consensus sequence for a vertex of the marker graph.
void Assembler::computeMarkerGraphVertexConsensusSequence(
    MarkerGraph::VertexId vertexId,
    vector<Base>& sequence,
    vector<uint32_t>& repeatCounts
    )
{

    // Access the markers of this vertex.
    const span<MarkerId> markerIds = markerGraph.getVertexMarkerIds(vertexId);
    const size_t markerCount = markerIds.size();
    SHASTA_ASSERT(markerCount > 0);

    // Find the corresponding oriented read ids, marker ordinals,
    // and marker positions in each oriented read id.
    vector< pair<OrientedReadId, uint32_t> > markerInfos;
    vector<uint32_t> markerPositions;
    markerInfos.reserve(markerIds.size());
    markerPositions.reserve(markerIds.size());
    for(const MarkerId markerId: markerIds) {
        markerInfos.push_back(findMarkerId(markerId));
        markerPositions.push_back(markers.begin()[markerId].position);
    }


    // Loop over all base positions of this marker.
    const size_t k = assemblerInfo->k;
    sequence.resize(k);
    repeatCounts.resize(k);
    for(uint32_t position=0; position<uint32_t(k); position++) {

        // Object to store base and repeat count information
        // at this position.
        Coverage coverage;

        // Loop over markers.
        for(size_t i=0; i<markerCount; i++) {

            // Get the base and repeat count.
            const OrientedReadId orientedReadId = markerInfos[i].first;
            const uint32_t markerPosition = markerPositions[i];
            Base base;
            uint8_t repeatCount;
            tie(base, repeatCount) = reads->getOrientedReadBaseAndRepeatCount(orientedReadId, markerPosition + position);

            // Add it to the Coverage object.
            coverage.addRead(AlignedBase(base), orientedReadId.getStrand(), size_t(repeatCount));
        }

        // Sanity check that all the bases are the same.
        const vector<CoverageData>& coverageData = coverage.getReadCoverageData();
        SHASTA_ASSERT(coverageData.size() == markerCount);
        const Base firstBase = Base(coverageData.front().base);
        for(const CoverageData& c: coverageData) {
            SHASTA_ASSERT(Base(c.base) == firstBase);
        }

        // Compute the consensus.
        const Consensus consensus = (*consensusCaller)(coverage);
        sequence[position] = Base(consensus.base);
        repeatCounts[position] = uint32_t(consensus.repeatCount);
    }
}



// Compute consensus sequence for an edge of the marker graph.
// This does not include the k bases corresponding to the flanking markers.
// If any of the marker intervals are longer than
// markerGraphEdgeLengthThresholdForConsensus, the consensus is not computed using spoa
// to avoid memory and performance problems.
// Instead, we return as consensus the sequence of the shortest marker interval.
// This should happen only exceptionally.
void Assembler::computeMarkerGraphEdgeConsensusSequenceUsingSpoa(
    MarkerGraph::EdgeId edgeId,
    uint32_t markerGraphEdgeLengthThresholdForConsensus,
    const std::unique_ptr<spoa::AlignmentEngine>& spoaAlignmentEngine,
    const std::unique_ptr<spoa::Graph>& spoaAlignmentGraph,
    vector<Base>& sequence,
    vector<uint32_t>& repeatCounts,
    uint8_t& overlappingBaseCount,
    ComputeMarkerGraphEdgeConsensusSequenceUsingSpoaDetail& detail,
    vector< pair<uint32_t, CompressedCoverageData> >* coverageData // Optional
    )
{
    // Flag to control debug output.
    const bool debug = false;

    // Get the marker length.
    const uint32_t k = uint32_t(assemblerInfo->k);

    // Access the markerIntervals for this edge.
    // Each corresponds to an oriented read on this edge.
    const span<MarkerInterval> markerIntervals =
        markerGraph.edgeMarkerIntervals[edgeId];
    const size_t markerCount = markerIntervals.size();
    SHASTA_ASSERT(markerCount > 0);



    // Find out if very long marker intervals are present.
    detail.hasLongMarkerInterval = false;
    for(size_t i=0; i!=markerCount; i++) {
        const MarkerInterval& markerInterval = markerIntervals[i];

        // Check the number of markers.
        const auto markerCount = markerInterval.ordinals[1] - markerInterval.ordinals[0];
        if(markerCount > markerGraphEdgeLengthThresholdForConsensus) {
            detail.hasLongMarkerInterval = true;
        }

        // Check the number of RLE bases.
        const span<CompressedMarker> orientedReadMarkers = markers[markerInterval.orientedReadId.getValue()];
        const CompressedMarker& marker0 = orientedReadMarkers[markerInterval.ordinals[0]];
        const CompressedMarker& marker1 = orientedReadMarkers[markerInterval.ordinals[1]];
        const uint64_t rleBaseCount = marker1.position - marker0.position;
        if(rleBaseCount > 1000) {       // *************** THIS SHOULD BE EXPOSED AS AN OPTION.
           detail.hasLongMarkerInterval = true;
        }
    }



    // If very long marker intervals are present, the computation of consensus sequence
    // would be prohibitive in memory and compute cost.
    // Just return as consensus the sequence of the shortest marker interval.
    if(detail.hasLongMarkerInterval) {

        // Find the shortest marker interval (number of markers).
        uint32_t minLength = std::numeric_limits<uint32_t>::max();
        detail.iShortest = 0;
        for(size_t i=0; i!=markerCount; i++) {
            const MarkerInterval& markerInterval = markerIntervals[i];
            const uint32_t length = markerInterval.ordinals[1] - markerInterval.ordinals[0]; // Number of markers.
            if(length < minLength) {
                minLength = length;
                detail.iShortest = i;
            }
        }
        const MarkerInterval& markerInterval = markerIntervals[detail.iShortest];
        const OrientedReadId orientedReadId = markerInterval.orientedReadId;
        const auto orientedReadMarkers = markers[orientedReadId.getValue()];

        // Get the two markers.
        const CompressedMarker& marker0 = orientedReadMarkers[markerInterval.ordinals[0]];
        const CompressedMarker& marker1 = orientedReadMarkers[markerInterval.ordinals[1]];

        // Get their positions.
        const uint32_t position0 = marker0.position;
        const uint32_t position1 = marker1.position;

        // Get the sequence.
        sequence.clear();
        repeatCounts.clear();
        if(coverageData) {
            coverageData->clear();
        }
        if(position1 > position0 + k) {
            for(uint32_t position=position0+k; position!=position1; position++) {

                if(assemblerInfo->readRepresentation == 1) {
                    // RLE
                    Base base;
                    uint32_t repeatCount;
                    tie(base, repeatCount) = reads->getOrientedReadBaseAndRepeatCount(orientedReadId, position);
                    sequence.push_back(base);
                    repeatCounts.push_back(repeatCount);
                    if(coverageData) {
                        CompressedCoverageData c;
                        c.base = base.value & 7;
                        c.strand = orientedReadId.getStrand() & 1;
                        c.repeatCount = uint8_t(min(uint32_t(255), repeatCount));
                        c.frequency = 1;
                        coverageData->push_back(make_pair(position - (position0+k), c));
                    }
                } else {
                    // Non-RLE.
                    const Base base = reads->getOrientedReadBase(orientedReadId, position);
                    sequence.push_back(base);
                }
            }
            overlappingBaseCount = 0;
        } else {
            overlappingBaseCount = uint8_t(position0 + k - position1);
        }

        // Done handling pathological case.
        return;
    }



    // We will process the edge in one of two modes:
    // - Mode 1 supports only marker intervals in which the left and right
    //   markers are adjacent or overlapping.
    // - Mode 2 supports only marker intervals in which there is at least
    //   one base of sequence intervening between the left and right
    //   markers.
    // It would be nice if we could find a mode that supports all of the
    // reads, but in general this is not possible. So we do the following:
    // - Count the number of marker intervals supported by mode 1 and mode 2.
    // - Pick the mode that supports the most marker intervals.
    // - Discard the marker intervals that are not supported by the mode we picked.
    // In the worst case, this results in discarding half of the marker intervals.
    // But in most cases, the number of marker intervals that must be discarded
    // is small, and often zero.
    // See comments below for details of the processing for each mode.
    size_t mode1Count = 0;
    size_t mode2Count = 0;
    for(size_t i=0; i!=markerCount; i++) {
        const MarkerInterval& markerInterval = markerIntervals[i];
        const OrientedReadId orientedReadId = markerInterval.orientedReadId;
        const auto orientedReadMarkers = markers[orientedReadId.getValue()];

        // Get the two markers.
        SHASTA_ASSERT(markerInterval.ordinals[1] > markerInterval.ordinals[0]);
        const CompressedMarker& marker0 = orientedReadMarkers[markerInterval.ordinals[0]];
        const CompressedMarker& marker1 = orientedReadMarkers[markerInterval.ordinals[1]];

        // Get the positions of the markers in the read.
        const uint32_t position0 = marker0.position;
        const uint32_t position1 = marker1.position;
        SHASTA_ASSERT(position1 > position0);

        // Compute the offset between the markers.
        const uint32_t offset = position1 - position0;

        // Update counts for mode 1 and mode 2.
        if(offset <= k) {
            // The markers are adjacent or overlapping.
            ++mode1Count;
        }
        if(offset > k) {
            // The markers are adjacent or non-overlapping.
            ++mode2Count;
        }
    }
    SHASTA_ASSERT(mode1Count + mode2Count == markerCount);



    // Mode 1: only use marker intervals in which the two markers
    // are adjacent or overlapping.
    // To construct the sequence, we use the most frequent
    // offset between the two markers.
    if(mode1Count >= mode2Count) {
        detail.assemblyMode = 1;

        // Offset histogram.
        // Count marker offsets up to k.
        // Since we are at it, also get the kmerIds.
        vector<uint32_t> offsetHistogram(k+1, 0);
        for(size_t i=0; i!=markerCount; i++) {
            const MarkerInterval& markerInterval = markerIntervals[i];
            const OrientedReadId orientedReadId = markerInterval.orientedReadId;
            const auto orientedReadMarkers = markers[orientedReadId.getValue()];

            // Get the two markers.
            SHASTA_ASSERT(markerInterval.ordinals[1] > markerInterval.ordinals[0]);
            const CompressedMarker& marker0 = orientedReadMarkers[markerInterval.ordinals[0]];
            const CompressedMarker& marker1 = orientedReadMarkers[markerInterval.ordinals[1]];

            // Get the positions of the markers in the read.
            const uint32_t position0 = marker0.position;
            const uint32_t position1 = marker1.position;
            SHASTA_ASSERT(position1 > position0);

            // Compute the offset between the markers.
            const uint32_t offset = position1 - position0;

            // Update the offset, but only if the markers are adjacent or overlapping.
            if(offset <= k) {
                ++offsetHistogram[offset];
            }
        }


        // Compute the most frequent offset.
        const uint32_t bestOffset = uint32_t(
            std::max_element(offsetHistogram.begin(), offsetHistogram.end())
            - offsetHistogram.begin());

        // Set the output accordingly.
        sequence.clear();
        repeatCounts.clear();
        overlappingBaseCount = uint8_t(k - bestOffset);
        if(coverageData) {
            coverageData->clear();
        }
        return;
    }



    // Mode 2: only use marker intervals in which there is at least
    // one base of sequence intervening between the left and right
    // markers.
    // We do a multiple sequence alignment using spoa
    // of the sequences between the two markers.
    // For performance, we enter each distinct sequence once,
    // with weight equal to its frequency.
    // Results from spoa are dependent on the order in which the
    // sequences are entered, and so we enter the sequences in order
    // of decreasing frequency.
    SHASTA_ASSERT(mode2Count > mode1Count);
    detail.assemblyMode = 2;



    // Gather all of the intervening sequences and repeatCounts, keeping track of distinct
    // sequences. For each sequence we store a vector of i values
    // where each sequence appear.
    vector< vector<Base> > distinctSequences;
    vector< vector<size_t> >& distinctSequenceOccurrences = detail.distinctSequenceOccurrences;
    distinctSequenceOccurrences.clear();
    vector<bool> isUsed(markerCount);
    vector<Base> interveningSequence;
    vector< vector<uint8_t> > interveningRepeatCounts(markerCount);
    for(size_t i=0; i!=markerCount; i++) {
        const MarkerInterval& markerInterval = markerIntervals[i];
        const OrientedReadId orientedReadId = markerInterval.orientedReadId;
        const auto orientedReadMarkers = markers[orientedReadId.getValue()];

        // Get the two markers and their positions.
        const CompressedMarker& marker0 = orientedReadMarkers[markerInterval.ordinals[0]];
        const CompressedMarker& marker1 = orientedReadMarkers[markerInterval.ordinals[1]];
        const uint32_t position0 = marker0.position;
        const uint32_t position1 = marker1.position;
        SHASTA_ASSERT(position1 > position0);
        const uint32_t offset = position1 - position0;

        // If the offset is too small, discard this marker interval.
        if(offset <= k) {
            isUsed[i] = false;
            continue;
        }
        isUsed[i] = true;

        // Construct the sequence and repeat counts between the markers.
        const uint32_t begin = position0 + k;
        const uint32_t end = position1;
        interveningSequence.clear();
        for(uint32_t position=begin; position!=end; position++) {

            if(assemblerInfo->readRepresentation == 1) {
                // RLE.
                Base base;
                uint8_t repeatCount;
                tie(base, repeatCount) = reads->getOrientedReadBaseAndRepeatCount(orientedReadId, position);
                interveningSequence.push_back(base);
                interveningRepeatCounts[i].push_back(repeatCount);
            } else {
                // Non-RLE.
                interveningSequence.push_back(reads->getOrientedReadBase(orientedReadId, position));
            }
        }

        if(debug) {
            cout << orientedReadId << endl;
            for(const Base base: interveningSequence) {
                cout << base;
            }
            cout << endl;
            for(const uint8_t repeatCount: interveningRepeatCounts[i]) {
                if(repeatCount < 10) {
                    cout << int(repeatCount);
                } else {
                    cout << "*";
                }
            }
            cout << endl;
        }

        // Store, making sure to check if we already encountered this sequence.
        const auto it = find(distinctSequences.begin(), distinctSequences.end(), interveningSequence);
        if(it == distinctSequences.end()) {
            // We did not already encountered this sequence.
            distinctSequences.push_back(interveningSequence);
            distinctSequenceOccurrences.resize(distinctSequenceOccurrences.size() + 1);
            distinctSequenceOccurrences.back().push_back(i);
        } else {
            // We already encountered this sequence,
            distinctSequenceOccurrences[it - distinctSequences.begin()].push_back(i);
        }
    }



    // We want to enter the distinct sequences in order of decreasing frequency,
    // so we create a table of distinct sequences.
    // For each pair stored:
    // first = index of the distinct sequence in distintSequenced, distinctSequenceOccurrences.
    // second = frequency.
    vector< pair<size_t, uint32_t> > distinctSequenceTable;
    for(size_t j=0; j<distinctSequences.size(); j++) {
        distinctSequenceTable.push_back(make_pair(j, distinctSequenceOccurrences[j].size()));
    }
    sort(distinctSequenceTable.begin(), distinctSequenceTable.end(),
        OrderPairsBySecondOnlyGreater<size_t, uint32_t>());

    if(debug) {
        cout << "Distinct sequences:" << endl;
        for(size_t i=0; i<distinctSequences.size(); i++) {
            const vector<Base>& distinctSequence = distinctSequences[i];
            cout << "Index in distinctSequences: " << i << endl;
            for(const Base base: distinctSequence) {
                cout << base;
            }
            cout << endl;
        }
        cout << "Distinct sequence table:" << endl;
        for(size_t i=0; i<distinctSequenceTable.size(); i++) {
            const size_t indexInDistinctSequences =  distinctSequenceTable[i].first;
            const size_t frequency = distinctSequenceTable[i].second;
            const vector<Base>& distinctSequence = distinctSequences[indexInDistinctSequences];
            cout << "Index in distinctSequenceTable: " << i << endl;
            cout << "Index in distinctSequences: " << indexInDistinctSequences << endl;
            cout << "Frequency: " << frequency << endl;
            for(const Base base: distinctSequence) {
                cout << base;
            }
            cout << endl;
        }
    }

    // Find the alignment row that will correspond to each oriented read.
    detail.alignmentRow.clear();
    detail.alignmentRow.resize(markerCount, -1);
    for(size_t i=0; i<distinctSequenceTable.size(); i++) {
        const size_t indexInDistinctSequences =  distinctSequenceTable[i].first;
        const vector<size_t>& occurrences = distinctSequenceOccurrences[indexInDistinctSequences];
        for(const size_t j: occurrences) {
            detail.alignmentRow[j] = int(i);
        }
    }


    // We are now ready to compute the spoa alignment for the distinct sequences.

    spoaAlignmentGraph->clear();
    // Add the sequences to the alignment, in order of decreasing frequency.
    string sequenceString;
    for(const auto& p: distinctSequenceTable) {
        const vector<Base>& distinctSequence = distinctSequences[p.first];

        // Add it to the alignment.
        sequenceString.clear();
        for(const Base base: distinctSequence) {
            sequenceString += base.character();
        }
        auto alignment = spoaAlignmentEngine->align(sequenceString, spoaAlignmentGraph);
        spoaAlignmentGraph->add_alignment(alignment, sequenceString);
    }



    // Use spoa to compute the multiple sequence alignment.
    vector<string>& msa = detail.msa;
    spoaAlignmentGraph->generate_multiple_sequence_alignment(msa);

    // The length of the alignment.
    // This includes alignment gaps.
    const size_t alignmentLength = msa.front().size();

    if(debug) {
        cout << "Spoa alignment:" << endl;
        for(size_t i=0; i<msa.size(); i++) {
            cout << msa[i] << endl;
        }
    }



    // Construct the edge sequence (and repeat counts if working in RLE).
    // We loop over all positions in the alignment.
    // At each position we compute a consensus base and repeat count.
    // If the consensus base is not '-', we store the base and repeat count.
    sequence.clear();
    repeatCounts.clear();
    detail.alignedConsensus.clear();
    detail.alignedRepeatCounts.clear();
    overlappingBaseCount = 0;
    if(coverageData) {
        coverageData->clear();
    }



    // We loop over all positions in the alignment.
    // At each position we compute a consensus base and repeat count.
    // If the consensus bases is not '-', we store the base and repeat count.
    vector<uint32_t> positions(markerCount, 0);
    for(size_t position=0; position<alignmentLength; position++) {

        if(debug) {
            cout << "Computing consensus repeat count at alignment position " << position << endl;
        }

        // Create a Coverage object for this position.
        Coverage coverage;

        // Loop over distinct sequences, in the same order in
        // which we presented them to spoa.
        for(size_t j=0; j<distinctSequenceTable.size(); j++) {
            const auto& p = distinctSequenceTable[j];
            const size_t index = p.first;
            // const vector<Base>& distinctSequence = distinctSequences[index];
            const vector<size_t>& occurrences = distinctSequenceOccurrences[index];

            // Loop over the marker intervals that have this sequence.
            for(const size_t i: occurrences) {
                const MarkerInterval& markerInterval = markerIntervals[i];
                const OrientedReadId orientedReadId = markerInterval.orientedReadId;
                const AlignedBase base = AlignedBase::fromCharacter(msa[j][position]);
                if(base.isGap()) {
                    coverage.addRead(base, orientedReadId.getStrand(), 0);
                    if(debug) {
                        cout << base << " " << 0 << " " << orientedReadId.getStrand() << endl;
                    }
                } else {
                    if(assemblerInfo->readRepresentation == 1) {
                        coverage.addRead(
                            base,
                            orientedReadId.getStrand(),
                            interveningRepeatCounts[i][positions[i]]);
                    } else {
                        coverage.addRead(
                            base,
                            orientedReadId.getStrand(),
                            1);
                    }
                    if(debug) {
                        cout << base << " " << int(interveningRepeatCounts[i][positions[i]]) << " " << orientedReadId.getStrand() << endl;
                    }
                    ++positions[i];
                }
            }
        }

        // Compute the consensus at this position.
        const Consensus consensus = (*consensusCaller)(coverage);

        // If not a gap, store the base and repeat count.
        if(!consensus.base.isGap()) {
            sequence.push_back(Base(consensus.base));
            SHASTA_ASSERT(consensus.repeatCount > 0);
            repeatCounts.push_back(uint32_t(consensus.repeatCount));

            // Also store detailed coverage data, if requested.
            if(coverageData) {
                vector<CompressedCoverageData> c;
                coverage.count(c);
                for(const CompressedCoverageData& cd: c) {
                    coverageData->push_back(make_pair(sequence.size()-1, cd));
                }
            }
        }

        // Also store aligned consensus.
        detail.alignedConsensus.push_back(consensus.base);
        uint8_t repeatCount;
        if(consensus.base.isGap()) {
            repeatCount = 0;
        } else {
            if(consensus.repeatCount < 256) {
                repeatCount = uint8_t(consensus.repeatCount);
            } else {
                repeatCount = 255;
            }
        }
        detail.alignedRepeatCounts.push_back(repeatCount);
    }

    if(debug) {
        cout << "Consensus:" << endl;
        for(const Base base: sequence) {
            cout << base;
        }
        cout << endl;
        for(const int repeatCount: repeatCounts) {
            if(repeatCount < 10) {
                cout << repeatCount;
            } else {
                cout << "*";
            }
        }
        cout << endl;
    }
}



// Simplify the marker graph.
// The first argument is a number of marker graph edges.
// See the code for detail on its meaning and how it is used.
// This never adds any edges. It only marks some edges
// as superbubble edges. Those edges will then be excluded from assembly.
// In the future we can also keep track of edges that were removed
// to generate alternative assembled sequence.
void Assembler::simplifyMarkerGraph(
    const vector<size_t>& maxLengthVector, // One value for each iteration.
    bool debug)
{
    // Clear the superbubble flag for all edges.
    for(MarkerGraph::Edge& edge: markerGraph.edges) {
        edge.isSuperBubbleEdge = 0;
    }



    // At each iteration we use a different maxLength value.
    for(size_t iteration=0; iteration<maxLengthVector.size(); iteration++) {
        const size_t maxLength = maxLengthVector[iteration];
        cout << timestamp << "Begin simplifyMarkerGraph iteration " << iteration <<
            " with maxLength = " << maxLength << endl;
        checkMarkerGraphIsStrandSymmetric();
        simplifyMarkerGraphIterationPart1(iteration, maxLength, debug);
        checkMarkerGraphIsStrandSymmetric();
        simplifyMarkerGraphIterationPart2(iteration, maxLength, debug);
    }
    checkMarkerGraphIsStrandSymmetric();



    // Count the marker graph vertices that are not isolated.
    size_t markerGraphVerticesNotIsolatedCount = 0;
    for(MarkerGraph::VertexId v=0; v!=markerGraph.vertexCount(); v++) {
        bool isIsolated = true;
        for(const MarkerGraph::EdgeId edgeId: markerGraph.edgesBySource[v]) {
            const MarkerGraph::Edge& edge = markerGraph.edges[edgeId];
            if(!edge.wasRemoved()) {
                isIsolated = false;
                break;
            }
        }
        if(isIsolated) {
            for(const MarkerGraph::EdgeId edgeId: markerGraph.edgesByTarget[v]) {
                const MarkerGraph::Edge& edge = markerGraph.edges[edgeId];
                if(!edge.wasRemoved()) {
                    isIsolated = false;
                    break;
                }
            }

        }
        if(!isIsolated) {
            ++markerGraphVerticesNotIsolatedCount;
        }
    }
    assemblerInfo->markerGraphVerticesNotIsolatedCount = markerGraphVerticesNotIsolatedCount;



    // Count the marker graph edges that were not removed.
    size_t markerGraphEdgesNotRemovedCount = 0;
    for(const MarkerGraph::Edge& edge: markerGraph.edges) {
        if(!edge.wasRemoved()) {
            ++markerGraphEdgesNotRemovedCount;
        }
    }
    assemblerInfo->markerGraphEdgesNotRemovedCount = markerGraphEdgesNotRemovedCount;
}



// Part 1 of each iteration: handle bubbles.
// For each set of parallel edges in the assembly graph in which all edges
// have at most maxLength markers, keep only the one with the highest average coverage.
void Assembler::simplifyMarkerGraphIterationPart1(
    size_t iteration,
    size_t maxLength,
    bool debug)
{
    // Setup debug output for this iteration, if requested.
    ofstream debugOut;
    if(debug) {
        debugOut.open("simplifyMarkerGraphIterationPart1-" + to_string(iteration) + ".debugLog");
    }

    // Create a temporary assembly graph.
    createAssemblyGraphEdges();
    createAssemblyGraphVertices();
    AssemblyGraph& assemblyGraph = *assemblyGraphPointer;
    if(debug) {
        assemblyGraph.writeGfa1BothStrandsNoSequence(
            "AssemblyGraph-BubbleRemoval-Iteration-" + to_string(iteration) + ".gfa");
        assemblyGraph.writeGraphviz("AssemblyGraph-simplifyMarkerGraphIterationPart1-" + to_string(iteration) + ".dot");
    }
    cout << "Before iteration " << iteration << " part 1, the assembly graph has " <<
        assemblyGraph.vertices.size() << " vertices and " <<
        assemblyGraph.edges.size() << " edges." << endl;



    // Loop over vertices in the assembly graph.
    vector<bool> keepAssemblyGraphEdge(assemblyGraph.edges.size(), true);
    for(AssemblyGraph::VertexId v0=0; v0<assemblyGraph.vertices.size(); v0++) {

        // Get edges that have this vertex as the source.
        const span<AssemblyGraph::EdgeId> outEdges = assemblyGraph.edgesBySource[v0];

        // If any of these edges have more than maxLength markers, do nothing.
        bool longEdgeExists = false;
        for(AssemblyGraph::EdgeId edgeId: outEdges) {
            if(assemblyGraph.edgeLists.size(edgeId) > maxLength) {
                longEdgeExists = true;
                break;
            }
        }
        if(longEdgeExists) {
            continue;
        }

        // Gather the out edges, for each target.
        // Map key = target vertex id
        // Map value: pairs(edgeId, average coverage).
        std::map<AssemblyGraph::VertexId, vector< pair<AssemblyGraph::EdgeId, uint32_t> > > edgeTable;
        for(AssemblyGraph::EdgeId edgeId: outEdges) {
            const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
            edgeTable[edge.target].push_back(make_pair(edgeId, edge.averageEdgeCoverage));
        }

        // For each set of parallel edges, only keep the one with the highest average coverage.
        for (auto& p : edgeTable) {
            const AssemblyGraph::VertexId v1 = p.first;
            if (v1 == assemblyGraph.reverseComplementVertex[v0]) {
                // v0 and v1 are reverse complement of each other: skip for now.
                continue;
            }
            vector< pair<AssemblyGraph::EdgeId, uint32_t> >& v = p.second;
            if(v.size() < 2) {
                continue;
            }
            sort(v.begin(), v.end(), OrderPairsBySecondOnlyGreater<AssemblyGraph::EdgeId, uint32_t>());
            for(auto it=v.begin()+1; it!=v.end(); ++it) {
                keepAssemblyGraphEdge[it->first] = false;
            }
            if(debug) {
                debugOut << "Parallel edges:\n";
                for(const auto& p: v) {
                    const AssemblyGraph::EdgeId edgeId = p.first;
                    const uint32_t averageCoverage = p.second;
                    debugOut << edgeId << " " << assemblyGraph.edgeLists.size(edgeId) <<
                        " " << averageCoverage << "\n";
                }
            }
        }
    }

    // Mark as superbubble edges all marker graph edges that correspond
    // to assembly graph edges not marked to be kept.
    // Whenever marking an edge, always also mark the reverse complemented edge,
    // so we keep the marker graph strand-symmetric.
    for(AssemblyGraph::EdgeId assemblyGraphEdgeId=0; assemblyGraphEdgeId<assemblyGraph.edges.size(); assemblyGraphEdgeId++) {
        if(keepAssemblyGraphEdge[assemblyGraphEdgeId]) {
            continue;
        }

        const span<MarkerGraph::EdgeId> markerGraphEdges = assemblyGraph.edgeLists[assemblyGraphEdgeId];
        for(const MarkerGraph::EdgeId markerGraphEdgeId: markerGraphEdges) {
            markerGraph.edges[markerGraphEdgeId].isSuperBubbleEdge = 1;
            markerGraph.edges[markerGraph.reverseComplementEdge[markerGraphEdgeId]].isSuperBubbleEdge = 1;
        }
    }



    // Create a csv file that can be loaded in Bandage to display all removed edges in gray.
    if(debug) {
        ofstream csv(
            "AssemblyGraph-BubbleRemoval-Iteration-" + to_string(iteration) + ".csv");
        csv << "EdgeId,Color,Source,Target\n";
        for(AssemblyGraph::EdgeId edgeId=0; edgeId<assemblyGraph.edges.size(); edgeId++) {
            csv << edgeId << ",";
            if(keepAssemblyGraphEdge[edgeId]) {
                csv << "green";
            } else {
                csv << "#D3D3D3";
            }
            const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
            csv << "," << assemblyGraph.vertices[edge.source] << ",";
            csv << assemblyGraph.vertices[edge.target] << "\n";
        }
    }



    // Remove the assembly graph we created at this iteration.
    assemblyGraph.remove();


}



// Part 2 of each iteration: handle superbubbles.
void Assembler::simplifyMarkerGraphIterationPart2(
    size_t iteration,
    size_t maxLength,
    bool debug)
{
    // Setup debug output for this iteration, if requested.
    ofstream debugOut;
    if(debug) {
        debugOut.open("simplifyMarkerGraphIterationPart2-" + to_string(iteration) + ".debugLog");
    }

    // Create a temporary assembly graph.
    createAssemblyGraphEdges();
    createAssemblyGraphVertices();
    AssemblyGraph& assemblyGraph = *assemblyGraphPointer;
    if(debug) {
        assemblyGraph.writeGfa1BothStrandsNoSequence(
            "AssemblyGraph-SuperBubbleRemoval-Iteration-" + to_string(iteration) + ".gfa");
        assemblyGraph.writeGraphviz("AssemblyGraph-simplifyMarkerGraphIterationPart2-" + to_string(iteration) + ".dot");
    }
    cout << "Before iteration " << iteration << " part 2, the assembly graph has " <<
        assemblyGraph.vertices.size() << " vertices and " <<
        assemblyGraph.edges.size() << " edges." << endl;



    // Compute connected components of the temporary assembly graph,
    // considering only edges with length (number of corresponding
    // marker graph edges) up to maxLength.
    // Initialize the disjoint set data structures.
    const size_t n = assemblyGraph.vertices.size();
    vector<AssemblyGraph::VertexId> rank(n);
    vector<AssemblyGraph::VertexId> parent(n);
    boost::disjoint_sets<AssemblyGraph::VertexId*, AssemblyGraph::VertexId*> disjointSets(&rank[0], &parent[0]);
    for(AssemblyGraph::VertexId vertexId=0; vertexId<n; vertexId++) {
        disjointSets.make_set(vertexId);
    }
    for(AssemblyGraph::EdgeId edgeId=0; edgeId<assemblyGraph.edges.size(); edgeId++) {
        if(assemblyGraph.edgeLists[edgeId].size() > maxLength) {
            continue;
        }
        const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
        disjointSets.union_set(edge.source, edge.target);
    }



    // Mark as to be kept all assembly graph edges in between components
    // or with length more than maxLength.
    vector<bool> keepAssemblyGraphEdge(assemblyGraph.edges.size(), false);
    for(AssemblyGraph::EdgeId edgeId=0; edgeId<assemblyGraph.edges.size(); edgeId++) {
        const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
        const AssemblyGraph::VertexId v0 = edge.source;
        const AssemblyGraph::VertexId v1 = edge.target;
        if((disjointSets.find_set(v0) != disjointSets.find_set(v1)) or
            (assemblyGraph.edgeLists[edgeId].size() > maxLength)) {
            keepAssemblyGraphEdge[edgeId] = true;
        }
    }


    // Gather the vertices in each connected component.
    // Note that, because of the way this is done, the vertex ids
    // in each component are sorted.
    vector< vector<AssemblyGraph::VertexId> > componentTable(n);
    for(AssemblyGraph::VertexId vertexId=0; vertexId<n; vertexId++) {
        componentTable[disjointSets.find_set(vertexId)].push_back(vertexId);
    }



    // The marker graph and the assembly graph are strand-symmetric, so
    // most components come in reverse complemented pairs,
    // and some are self-complementary.
    // Find the pairs.
    vector< AssemblyGraph::VertexId > rcComponentTable(n);
    for(AssemblyGraph::VertexId componentId=0; componentId<n; componentId++) {

        // Get the assembly graph vertices in this connected component
        // and skip it if it is empty.
        const vector<AssemblyGraph::VertexId>& component = componentTable[componentId];
        if(component.empty()) {
            continue;
        }

        // Find the reverse complement of the first vertex.
        const AssemblyGraph::VertexId v = component.front();
        const AssemblyGraph::VertexId vRc = assemblyGraph.reverseComplementVertex[v];
        const AssemblyGraph::VertexId componentRcId = disjointSets.find_set(vRc);

        rcComponentTable[componentId] = componentRcId;
    }

    // Sanity checks.
    for (AssemblyGraph::VertexId componentId = 0; componentId < n; componentId++) {
        const vector<AssemblyGraph::VertexId>& component = componentTable[componentId];
        if (component.empty()) {
            continue;
        }
        const AssemblyGraph::VertexId componentRcId = rcComponentTable[componentId];
        SHASTA_ASSERT(rcComponentTable[componentRcId] == componentId);
        if (componentRcId == componentId) {
            cout << "Found a self-complementary component with " << component.size() << " vertices." << endl;
        }
    }

    // More sanity checks.
    for (AssemblyGraph::VertexId v0 = 0; v0 < n; v0++) {
        const AssemblyGraph::VertexId v1 = assemblyGraph.reverseComplementVertex[v0];
        const AssemblyGraph::VertexId c0 = disjointSets.find_set(v0);
        const AssemblyGraph::VertexId c1 = disjointSets.find_set(v1);
        SHASTA_ASSERT(rcComponentTable[c0] == c1);
        SHASTA_ASSERT(rcComponentTable[c1] == c0);
    }




    // Find entries and exits.
    // An entry is a vertex with an in-edge from another component,
    // or an in-edge longer than maxLength.
    // An exit is a vertex with an out-edge to another component,
    // or an out-edge longer than maxLength.
    vector<bool> isEntry(n, false);
    vector<bool> isExit(n, false);
    for(AssemblyGraph::VertexId v0=0; v0<n; v0++) {
        const AssemblyGraph::VertexId componentId0 = disjointSets.find_set(v0);
        const span<AssemblyGraph::EdgeId> inEdges = assemblyGraph.edgesByTarget[v0];
        for(AssemblyGraph::EdgeId edgeId : inEdges) {
            if(assemblyGraph.edgeLists.size(edgeId) > maxLength) {
                isEntry[v0] = true;
                break;
            }
            const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
            SHASTA_ASSERT(edge.target == v0);
            const AssemblyGraph::VertexId componentId1 = disjointSets.find_set(edge.source);
            if(componentId1 != componentId0) {
                isEntry[v0] = true;
                break;
            }
        }
        const span<AssemblyGraph::EdgeId> outEdges = assemblyGraph.edgesBySource[v0];
        for(AssemblyGraph::EdgeId edgeId : outEdges) {
            if(assemblyGraph.edgeLists.size(edgeId) > maxLength) {
                isExit[v0] = true;
                break;
            }
            const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
            SHASTA_ASSERT(edge.source == v0);
            const AssemblyGraph::VertexId componentId1 = disjointSets.find_set(edge.target);
            if(componentId1 != componentId0) {
                isExit[v0] = true;
                break;
            }
        }
    }



#if 0
    // Work areas used below.
    // Allocate them here to reduce memory allocation activity.
    vector<AssemblyGraph::EdgeId> predecessorEdge(n);
    vector<uint8_t> color(n);
#endif




    // Process one connected component at a time.
    for(AssemblyGraph::VertexId componentId=0; componentId<n; componentId++) {

        // Get the assembly graph vertices in this connected component
        // and skip it if it is empty.
        const vector<AssemblyGraph::VertexId>& component = componentTable[componentId];
        if(component.empty()) {
            continue;
        }

        if(debug) {
            debugOut << "\nProcessing connected component with " << component.size() <<
                " assembly/marker graph vertices:" << "\n";
            for(const AssemblyGraph::VertexId assemblyGraphVertexId: component) {
                const MarkerGraph::VertexId markerGraphVertexId = assemblyGraph.vertices[assemblyGraphVertexId];
                debugOut << assemblyGraphVertexId << "/" << markerGraphVertexId;
                if(isEntry[assemblyGraphVertexId]) {
                    debugOut << " entry";
                }
                if(isExit[assemblyGraphVertexId]) {
                    debugOut << " exit";
                }
                debugOut << "\n";
            }
        }



        // If this component is self-complementary, it requires special handling.
        // Skip for now.
        if(rcComponentTable[componentId] == componentId) {
        	cout << "Skipped a self-complementary component with " <<
        		component.size() << " vertices." << endl;
            for(const AssemblyGraph::VertexId v0: component) {
                const AssemblyGraph::VertexId componentId0 = disjointSets.find_set(v0);
                const span<AssemblyGraph::EdgeId> outEdges = assemblyGraph.edgesBySource[v0];
                for(AssemblyGraph::EdgeId edgeId : outEdges) {
                    const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
                    SHASTA_ASSERT(edge.source == v0);
                    const AssemblyGraph::VertexId componentId1 = disjointSets.find_set(edge.target);
                    if(componentId1 == componentId0) {
                        keepAssemblyGraphEdge[edgeId] = true;
                    }
                }
            }
            continue;
        }

        // This component is not self complementary.
        // We want to handle each pair of components in the same way.
        // Only process one of the two in each pair.
        if(rcComponentTable[componentId] < componentId) {
            if(debug) {
                debugOut << "Skipped - reverse complement component will be processed." << endl;
            }
            continue;
        }



        // Find out if this component has any entries/exits.
        bool entriesExist = false;
        for(const AssemblyGraph::VertexId assemblyGraphVertexId: component) {
            if(isEntry[assemblyGraphVertexId]) {
                entriesExist = true;
                break;
            }
        }
        bool exitsExist = false;
        for(const AssemblyGraph::VertexId assemblyGraphVertexId: component) {
            if(isExit[assemblyGraphVertexId]) {
                exitsExist = true;
                break;
            }
        }



        // Handle the case where there are no entries or no exits.
        // This means that this component is actually an entire connected component
        // of the full assembly graph (counting all edges).
        if(!(entriesExist && exitsExist)) {
            if(debug) {
                debugOut << "Component skipped because it has no entries or no exits.\n";
                debugOut << "Due to this, the following edges will be kept:\n";
            }
            for(const AssemblyGraph::VertexId v0: component) {
                const AssemblyGraph::VertexId componentId0 = disjointSets.find_set(v0);
                const span<AssemblyGraph::EdgeId> outEdges = assemblyGraph.edgesBySource[v0];
                for(AssemblyGraph::EdgeId edgeId : outEdges) {
                    const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
                    SHASTA_ASSERT(edge.source == v0);
                    const AssemblyGraph::VertexId componentId1 = disjointSets.find_set(edge.target);
                    if(componentId1 == componentId0) {
                        keepAssemblyGraphEdge[edgeId] = true;
                        keepAssemblyGraphEdge[assemblyGraph.reverseComplementEdge[edgeId]] = true;
                        if(debug) {
                            debugOut << edgeId << "\n";
                        }
                    }
                }
            }
            continue;
        }


        // The code below relies on the vertex ids in the component vector to be sorted.
        // Check for that.
        SHASTA_ASSERT(std::is_sorted(component.begin(), component.end()));



        // Create a Boost graph to represent this component.
        // Vertex descriptors of this graph are indexes into the component vector.
        // This graph will be used below to compute shortest path,
        // with edge length defined as the inverse of average edge coverage.
        using boost::adjacency_list;
        using boost::listS;
        using boost::vecS;
        using boost::directedS;
        using boost::property;
        using boost::no_property;
        using boost::edge_weight_t;

        using Graph = adjacency_list<listS, vecS, directedS, no_property, property<edge_weight_t, double> >;
        using vertex_descriptor = Graph::vertex_descriptor;
        using edge_descriptor = Graph::edge_descriptor;

        Graph graph(component.size());
        auto weightMap = boost::get(boost::edge_weight, graph);
        for(uint64_t v0=0; v0<component.size(); v0++) {
            const AssemblyGraph::VertexId vertexId0 = component[v0];
            const span<AssemblyGraph::EdgeId> outEdges = assemblyGraph.edgesBySource[vertexId0];
            for(const AssemblyGraph::EdgeId edgeId: outEdges) {
                const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];

                // If the edge was removed, don't use it.
                if(edge.wasRemoved()) {
                    continue;
                }

                // If the edge is long, it should not be considered internal to the component.
                if(assemblyGraph.edgeLists[edgeId].size() > maxLength) {
                    continue;
                }

                const AssemblyGraph::VertexId vertexId1 = edge.target;

                // Look up vertexId1 in the component vector.
                // This gives us the vertex descriptor for the target vertex.
                const auto it = std::lower_bound(component.begin(), component.end(), vertexId1);
                if(*it != vertexId1) {
                    // This edge goes outside this component.
                    continue;
                }
                const vertex_descriptor v1 = it - component.begin();

                // Add the edge and set its weight.
                edge_descriptor e;
                tie(e, ignore) = add_edge(v0, v1, graph);
                weightMap[e] = 1. / double(edge.averageEdgeCoverage);
            }
        }
        if(debug) {
            BGL_FORALL_EDGES(e, graph, Graph) {
                const auto v0 = source(e, graph);
                const auto v1 = target(e, graph);
                debugOut << v0 << "->" << v1 << " " <<
                    component[v0] << "->" << component[v1] << " " <<
                    weightMap[e] << endl;
            }
        }


#if 0
        // Work areas used for shortest path computation.
        std::priority_queue<
            pair<float, AssemblyGraph::VertexId>,
            vector<pair<float, AssemblyGraph::VertexId> >,
            OrderPairsByFirstOnlyGreater<size_t, AssemblyGraph::VertexId> > q;
        vector< pair<float, AssemblyGraph::EdgeId> > sortedOutEdges;
#endif


        // Loop over entry/exit pairs.
        // We already checked that there is at least one entry
        // and one exit, so the inner body of this loop
        // gets executed at least once.
        for(uint64_t entryIndex=0; entryIndex<component.size(); entryIndex++) {
            const AssemblyGraph::VertexId entryId = component[entryIndex];
            if(!isEntry[entryId]) {
                continue;
            }

            if(debug) {
                debugOut << "Computing shortest paths starting at " <<
                    entryId << "/" << assemblyGraph.vertices[entryId] << "\n";
            }

            // Compute shortest paths
            // from this vertex to all other vertices in this component.
            using boost::make_iterator_property_map;
            using boost::get;
            using boost::vertex_index;
            using boost::dijkstra_shortest_paths_no_color_map;
            using boost::predecessor_map;
            vector< vertex_descriptor > predecessor(component.size());
            auto predecessorMap = make_iterator_property_map(predecessor.begin(), get(vertex_index, graph));
            dijkstra_shortest_paths_no_color_map(graph, entryIndex, predecessor_map(predecessorMap));
            if(debug) {
                debugOut << "Predecessor map:" << endl;
                for(vertex_descriptor v=0; v<component.size(); v++) {
                    debugOut << component[v] << " predecessor is " <<
                        component[predecessor[v]] << endl;
                }
            }

#if 0

            // Compute shortest paths
            // from this vertex to all other vertices in this component.
            // Use as edge weight the inverse of average coverage,
            // so the path prefers high coverage.
            SHASTA_ASSERT(q.empty());
            q.push(make_pair(0., entryId));
            for(const AssemblyGraph::VertexId v: component) {
                color[v] = 0;
                predecessorEdge[v] = AssemblyGraph::invalidEdgeId;
            }
            color[entryId] = 1;
            const AssemblyGraph::VertexId entryComponentId = disjointSets.find_set(entryId);
            while(!q.empty()) {

                // Dequeue.
                const pair<float, AssemblyGraph::VertexId> p = q.top();
                const float distance0 = p.first;
                const AssemblyGraph::VertexId v0 = p.second;
                q.pop();
                if(debug) {
                    debugOut << "Dequeued " << v0 << "/" << assemblyGraph.vertices[v0] <<
                        " at distance " << distance0 << "\n";
                }
                SHASTA_ASSERT(color[v0] == 1);

                // Find the out edges and sort them.
                const span<AssemblyGraph::EdgeId> outEdges = assemblyGraph.edgesBySource[v0];
                sortedOutEdges.clear();
                for(const AssemblyGraph::EdgeId e01: outEdges) {
                    sortedOutEdges.push_back(make_pair(1./assemblyGraph.edges[e01].averageEdgeCoverage, e01));
                }
                sort(sortedOutEdges.begin(), sortedOutEdges.end(),
                    OrderPairsByFirstOnly<double, AssemblyGraph::EdgeId>());

                // Loop over out-edges internal to this component.
                for(const pair<float, AssemblyGraph::EdgeId>& edgePair: sortedOutEdges) {
                    const AssemblyGraph::EdgeId e01 = edgePair.second;
                    const float length01 = edgePair.first;
                    const AssemblyGraph::VertexId v1 = assemblyGraph.edges[e01].target;
                    if(disjointSets.find_set(v1) != entryComponentId) {
                        continue;
                    }
                    if(color[v1] == 1) {
                        continue;
                    }
                    color[v1] = 1;
                    predecessorEdge[v1] = e01;
                    const float distance1 = distance0 + length01;
                    q.push(make_pair(distance1, v1));
                    if(debug) {
                        debugOut << "Enqueued " << v1 << "/" << assemblyGraph.vertices[v1] <<
                            " at distance " << distance1 << "\n";
                    }
                }
            }
#endif


            // Loop over all exits.
            for(uint64_t exitIndex=0; exitIndex<component.size(); exitIndex++) {
                const AssemblyGraph::VertexId exitId = component[exitIndex];
                if(!isExit[exitId]) {
                    continue;
                }
                if(exitId == entryId) {
                    continue;
                }

                if(predecessor[exitIndex] == exitIndex) {
                    continue;   // This exit is not reachable from this entry.
                }

                if(debug) {
                    debugOut << "The following assembly graph edges will be kept because they are "
                        "on the shortest path between entry " << entryId << "/" <<
                        assemblyGraph.vertices[entryId] <<
                        " and exit " << exitId << "/" << assemblyGraph.vertices[exitId] << "\n";
                }



                // Mark all the edges on the shortest path from this entry to this exit.
                // Walk the path backward using the predecessor tree.
                Graph::vertex_descriptor v1 = exitIndex;
                while(true) {
                    const Graph::vertex_descriptor v0 = predecessor[v1];

                    // Find the shortest edge v0->v1.
                    if(debug) {
                        debugOut << "Looking for best edge " << component[v0] << "->" << component[v1] << endl;
                    }
                    double bestCoverage = 0.;
                    AssemblyGraph::EdgeId bestEdgeId = std::numeric_limits<AssemblyGraph::EdgeId>::max();
                    const AssemblyGraph::VertexId vertexId0 = component[v0];
                    const span<AssemblyGraph::EdgeId> outEdges = assemblyGraph.edgesBySource[vertexId0];
                    for(const AssemblyGraph::EdgeId edgeId: outEdges) {
                        const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
                        if(edge.wasRemoved()) {
                            continue;
                        }
                        const AssemblyGraph::VertexId vertexId1 = edge.target;
                        if(vertexId1 != component[v1]) {
                            continue;
                        }
                        if(assemblyGraph.edgeLists[edgeId].size() > maxLength) {
                            continue;
                        }

                        if(edge.averageEdgeCoverage > bestCoverage) {
                            bestCoverage = edge.averageEdgeCoverage;
                            bestEdgeId = edgeId;
                        }
                    }
                    SHASTA_ASSERT(bestCoverage != 0);
                    if(debug) {
                        const AssemblyGraph::Edge& bestEdge = assemblyGraph.edges[bestEdgeId];
                        debugOut << "Best edge found " << bestEdge.source << "->" << bestEdge.target << endl;
                    }

                    // Mark the best edge and its reverse complement.
                    keepAssemblyGraphEdge[bestEdgeId] = true;
                    const AssemblyGraph::EdgeId bestEdgeIdReverseComplement =
                        assemblyGraph.reverseComplementEdge[bestEdgeId];
                    keepAssemblyGraphEdge[bestEdgeIdReverseComplement] = true;
                    if(debug) {
                        const AssemblyGraph::Edge& bestEdge = assemblyGraph.edges[bestEdgeId];
                        const AssemblyGraph::Edge& bestEdgeReverseComplement = assemblyGraph.edges[bestEdgeIdReverseComplement];
                        debugOut << "Marking " << bestEdge.source << "->" << bestEdge.target << " and ";
                        debugOut << bestEdgeReverseComplement.source << "->" << bestEdgeReverseComplement.target << endl;
                    }

                    // See if we reached the entry.
                    if(component[v0] == entryId) {
                        break;
                    }

                    // Go one edge back in the path.
                    v1 = v0;
                }

#if 0
                AssemblyGraph::VertexId v = exitId;
                while(true) {
                    AssemblyGraph::EdgeId e = predecessorEdge[v];
                    keepAssemblyGraphEdge[e] = true;
                    // Also keep the reverse complement. This keeps the assembly and marker graph symmetric.
                    keepAssemblyGraphEdge[assemblyGraph.reverseComplementEdge[e]] = true;
                    if(debug) {
                        debugOut << e << endl;
                    }
                    SHASTA_ASSERT(e != AssemblyGraph::invalidEdgeId);
                    v = assemblyGraph.edges[e].source;
                    if(v == entryId) {
                        break;
                    }
                }
                if(debug) {
                    debugOut << "\n";
                }
#endif
            }
        }
    }



    // Mark as superbubble edges all marker graph edges that correspond
    // to assembly graph edges not marked to be kept.
    for(AssemblyGraph::EdgeId assemblyGraphEdgeId=0; assemblyGraphEdgeId<assemblyGraph.edges.size(); assemblyGraphEdgeId++) {
        if(keepAssemblyGraphEdge[assemblyGraphEdgeId]) {
            continue;
        }

        const span<MarkerGraph::EdgeId> markerGraphEdges = assemblyGraph.edgeLists[assemblyGraphEdgeId];
        for(const MarkerGraph::EdgeId markerGraphEdgeId: markerGraphEdges) {
            markerGraph.edges[markerGraphEdgeId].isSuperBubbleEdge = 1;
        }
    }



    // Create a csv file that can be loaded in Bandage to display all removed edges in gray.
    // To do that:
    // 1. Start Bandage.
    // 2. File, Load Graph, select the gfa file with the same name as this csv file.
    // 3. File, Load csv data, select this file.
    // 4. Under "Graph display", select "Custom colors".
    // 5. Under "Node labels", uncheck "Csv data".
    // The file also contains the source and target marker graph vertex
    // for each assembly graph edge.
    if(debug) {
        ofstream csv(
            "AssemblyGraph-SuperBubbleRemoval-Iteration-" + to_string(iteration) + ".csv");
        csv << "EdgeId,Color,Source,Target\n";
        for(AssemblyGraph::EdgeId edgeId=0; edgeId<assemblyGraph.edges.size(); edgeId++) {
            csv << edgeId << ",";
            if(keepAssemblyGraphEdge[edgeId]) {
                csv << "green";
            } else {
                csv << "#D3D3D3";
            }
            const AssemblyGraph::Edge& edge = assemblyGraph.edges[edgeId];
            csv << "," << assemblyGraph.vertices[edge.source] << ",";
            csv << assemblyGraph.vertices[edge.target] << "\n";
        }
    }



    // Remove the assembly graph we created at this iteration.
    assemblyGraph.remove();

}



// Compute consensus repeat counts for each vertex of the marker graph.
void Assembler::assembleMarkerGraphVertices(size_t threadCount)
{
    cout << timestamp << "assembleMarkerGraphVertices begins." << endl;

    // Thus should only be called when using reads in RLE representation.
    SHASTA_ASSERT(assemblerInfo->readRepresentation == 1);

    // Check that we have what we need.
    checkKmersAreOpen();
    reads->checkReadsAreOpen();
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Initialize the vector to contain assemblerInfo->k optimal repeat counts for each vertex.
    markerGraph.vertexRepeatCounts.createNew(
        largeDataName("MarkerGraphVertexRepeatCounts"),
        largeDataPageSize);
    markerGraph.vertexRepeatCounts.resize(assemblerInfo->k * markerGraph.vertexCount());

    // Do the work in parallel.
    size_t batchSize = 100000;
    setupLoadBalancing(markerGraph.vertexCount(), batchSize);
    runThreads(&Assembler::assembleMarkerGraphVerticesThreadFunction, threadCount);

    cout << timestamp << "assembleMarkerGraphVertices ends." << endl;
}



void Assembler::assembleMarkerGraphVerticesThreadFunction(size_t threadId)
{
    vector<Base> sequence;
    vector<uint32_t> repeatCounts;
    const size_t k = assemblerInfo->k;

    // Loop over batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // Loop over marker graph vertices assigned to this batch.
        for(MarkerGraph::VertexId vertexId=begin; vertexId!=end; vertexId++) {

            // Compute the optimal repeat counts for this vertex.
            computeMarkerGraphVertexConsensusSequence(vertexId, sequence, repeatCounts);

            // Store them.
            SHASTA_ASSERT(repeatCounts.size() == k);
            copy(repeatCounts.begin(), repeatCounts.end(),
                markerGraph.vertexRepeatCounts.begin() + vertexId * k);
        }
    }
}



void Assembler::accessMarkerGraphVertexRepeatCounts()
{
    markerGraph.vertexRepeatCounts.accessExistingReadOnly(
        largeDataName("MarkerGraphVertexRepeatCounts"));

}



// Optional computation of coverage data for marker graph vertices.
// This is only called if Assembly.storeCoverageData in shasta.conf is True.
void Assembler::computeMarkerGraphVerticesCoverageData(size_t threadCount)
{
    cout << timestamp<< "computeMarkerGraphVerticesCoverageData begins." << endl;

    // Check that we have what we need.
    checkKmersAreOpen();
    reads->checkReadsAreOpen();
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Resize the data structures to contain results computed by each thread.
    computeMarkerGraphVerticesCoverageDataData.threadVertexIds.resize(threadCount);
    computeMarkerGraphVerticesCoverageDataData.threadVertexCoverageData.resize(threadCount);

    // Do the computation in parallel.
    setupLoadBalancing(markerGraph.vertexCount(), 100000);
    runThreads(&Assembler::computeMarkerGraphVerticesCoverageDataThreadFunction, threadCount);

    // Figure out where the results for each vertex are.
    // For each vertex we store pair(threadId, index in thread).
    // This vertex table can get big and we should store it
    // in a MemoryMapped::Vector instead.
    const size_t invalidValue = std::numeric_limits<size_t>::max();
    vector< pair<size_t, size_t > > vertexTable(
        markerGraph.vertexCount(),
        make_pair(invalidValue, invalidValue));
    for(size_t threadId=0; threadId!=threadCount; threadId++) {
        const auto& vertexIds = *computeMarkerGraphVerticesCoverageDataData.threadVertexIds[threadId];
        for(size_t i=0; i<vertexIds.size(); i++) {
            vertexTable[vertexIds[i]] = make_pair(threadId, i);
        }
    }



    // Gather the results computed by all the threads.
    markerGraph.vertexCoverageData.createNew(
        largeDataName("MarkerGraphVerticesCoverageData"), largeDataPageSize);
    for(MarkerGraph::VertexId vertexId=0; vertexId!=markerGraph.vertexCount(); vertexId++) {
        const auto& p = vertexTable[vertexId];
        const size_t threadId = p.first;
        const size_t i = p.second;
        SHASTA_ASSERT(threadId != invalidValue);
        SHASTA_ASSERT(i != invalidValue);
        const auto v = (*computeMarkerGraphVerticesCoverageDataData.threadVertexCoverageData[threadId])[i];
        markerGraph.vertexCoverageData.appendVector(v.begin(), v.end());
    }

    markerGraph.vertexCoverageData.unreserve();
    
    // Remove the results computed by each thread.
    for(size_t threadId=0; threadId!=threadCount; threadId++) {
        computeMarkerGraphVerticesCoverageDataData.threadVertexIds[threadId]->remove();
        computeMarkerGraphVerticesCoverageDataData.threadVertexCoverageData[threadId]->remove();
    }
    computeMarkerGraphVerticesCoverageDataData.threadVertexIds.clear();
    computeMarkerGraphVerticesCoverageDataData.threadVertexCoverageData.clear();

    cout << timestamp<< "computeMarkerGraphVerticesCoverageData ends." << endl;
}



void Assembler::computeMarkerGraphVerticesCoverageDataThreadFunction(size_t threadId)
{

    // Allocate space for the results computed by this thread.
    ComputeMarkerGraphVerticesCoverageDataData& data = computeMarkerGraphVerticesCoverageDataData;
    data.threadVertexIds[threadId] =
        make_shared< MemoryMapped::Vector<MarkerGraph::VertexId> >();
    data.threadVertexCoverageData[threadId] =
        make_shared< MemoryMapped::VectorOfVectors<pair<uint32_t, CompressedCoverageData>, uint64_t> >();
    auto& threadVertexIds = *data.threadVertexIds[threadId];
    auto& threadCoverageData = *data.threadVertexCoverageData[threadId];
    threadVertexIds.createNew(
        largeDataName("tmp-computeMarkerGraphVertices-vertexIds" + to_string(threadId)), largeDataPageSize);
    threadCoverageData.createNew(
        largeDataName("tmp-markerGraphVerticesCoverageData" + to_string(threadId)), largeDataPageSize);


    // Some work areas used in the loop and defined here to reduce memory allocation
    // activity.
    vector< pair<OrientedReadId, uint32_t> > markerInfos;
    vector<uint32_t> markerPositions;
    vector<CompressedCoverageData> compressedCoverageData;

    // Loop over all batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {

        // Loop over all vertices of this batch.
        for(MarkerGraph::VertexId vertexId=begin; vertexId!=end; vertexId++) {

            // Access the markers of this vertex.
            const span<MarkerId> markerIds = markerGraph.getVertexMarkerIds(vertexId);
            const size_t markerCount = markerIds.size();
            SHASTA_ASSERT(markerCount > 0);

            // Find the corresponding oriented read ids, marker ordinals,
            // and marker positions in each oriented read id.
            markerInfos.clear();
            markerPositions.clear();
            for(const MarkerId markerId: markerIds) {
                markerInfos.push_back(findMarkerId(markerId));
                markerPositions.push_back(markers.begin()[markerId].position);
            }

            // Loop over the k base positions in this vertex.
            threadVertexIds.push_back(vertexId);
            threadCoverageData.appendVector();
            for(uint32_t position=0; position<uint32_t(assemblerInfo->k); position++) {

                // Object to store base and repeat count information
                // at this position.
                Coverage coverage;

                // Loop over markers.
                for(size_t i=0; i<markerCount; i++) {

                    // Get the base and repeat count.
                    const OrientedReadId orientedReadId = markerInfos[i].first;
                    const uint32_t markerPosition = markerPositions[i];
                    Base base;
                    uint8_t repeatCount;
                    tie(base, repeatCount) = reads->getOrientedReadBaseAndRepeatCount(orientedReadId, markerPosition + position);

                    // Add it to the Coverage object.
                    coverage.addRead(AlignedBase(base), orientedReadId.getStrand(), size_t(repeatCount));
                }

                // Sanity check that all the bases are the same.
                const vector<CoverageData>& coverageData = coverage.getReadCoverageData();
                SHASTA_ASSERT(coverageData.size() == markerCount);
                const Base firstBase = Base(coverageData.front().base);
                for(const CoverageData& c: coverageData) {
                    SHASTA_ASSERT(Base(c.base) == firstBase);
                }

                // Store the results.
                coverage.count(compressedCoverageData);
                for(const CompressedCoverageData& cd: compressedCoverageData) {
                    threadCoverageData.append(make_pair(position, cd));
                }
            }
        }
    }

    threadVertexIds.unreserve();
    threadCoverageData.unreserve();

}



// Assemble consensus sequence and repeat counts for each marker graph edge.
void Assembler::assembleMarkerGraphEdges(
    size_t threadCount,

    // This controls when we give up trying to compute consensus for long edges.
    uint32_t markerGraphEdgeLengthThresholdForConsensus,

    // Request storing detailed coverage information.
    bool storeCoverageData,

    // Request assembling all edges (used by Mode 2 assembly)
    bool assembleAllEdges
    )
{
    cout << timestamp << "assembleMarkerGraphEdges begins." << endl;

    // Check that we have what we need.
    checkKmersAreOpen();
    reads->checkReadsAreOpen();
    checkMarkersAreOpen();
    checkMarkerGraphVerticesAreAvailable();
    checkMarkerGraphEdgesIsOpen();

    // Adjust the numbers of threads, if necessary.
    if(threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }

    // Do the computation in parallel.
    assembleMarkerGraphEdgesData.markerGraphEdgeLengthThresholdForConsensus = markerGraphEdgeLengthThresholdForConsensus;
    assembleMarkerGraphEdgesData.storeCoverageData = storeCoverageData;
    assembleMarkerGraphEdgesData.assembleAllEdges = assembleAllEdges;
    assembleMarkerGraphEdgesData.threadEdgeIds.resize(threadCount);
    assembleMarkerGraphEdgesData.threadEdgeConsensus.resize(threadCount);
    assembleMarkerGraphEdgesData.threadEdgeConsensusOverlappingBaseCount.resize(threadCount);
    if(storeCoverageData) {
        assembleMarkerGraphEdgesData.threadEdgeCoverageData.resize(threadCount);
    }
    // The batch size should not be too big, to avoid loss of parallelism
    // in small assemblies with high coverage (see discussion in issue #70).
    const size_t batchSize = 10;
    setupLoadBalancing(markerGraph.edges.size(), batchSize);
    runThreads(&Assembler::assembleMarkerGraphEdgesThreadFunction, threadCount);


    // Figure out where the results for each edge are.
    // For each edge we store pair(threadId, index in thread).
    // This edge table can get big and we should store it
    // in a MemoryMapped::Vector instead.
    const size_t invalidValue = std::numeric_limits<size_t>::max();
    vector< pair<size_t, size_t > > edgeTable(markerGraph.edges.size(), make_pair(invalidValue, invalidValue));
    for(size_t threadId=0; threadId!=threadCount; threadId++) {
        const auto& edgeIds = *assembleMarkerGraphEdgesData.threadEdgeIds[threadId];
        for(size_t i=0; i<edgeIds.size(); i++) {
            edgeTable[edgeIds[i]] = make_pair(threadId, i);
        }
    }

    // Gather the results.
    markerGraph.edgeConsensus.createNew(
        largeDataName("MarkerGraphEdgesConsensus"), largeDataPageSize);
    markerGraph.edgeConsensusOverlappingBaseCount.createNew(
        largeDataName("MarkerGraphEdgesConsensusOverlappingBaseCount"), largeDataPageSize);
    markerGraph.edgeConsensusOverlappingBaseCount.resize(markerGraph.edges.size());
    if(storeCoverageData) {
        markerGraph.edgeCoverageData.createNew(
            largeDataName("MarkerGraphEdgesCoverageData"), largeDataPageSize);
    }
    for(MarkerGraph::EdgeId edgeId=0; edgeId!=markerGraph.edges.size(); edgeId++) {
        const auto& p = edgeTable[edgeId];
        const size_t threadId = p.first;
        const size_t i = p.second;
        SHASTA_ASSERT(threadId != invalidValue);
        SHASTA_ASSERT(i != invalidValue);
        const auto& results = (*assembleMarkerGraphEdgesData.threadEdgeConsensus[threadId])[i];
        markerGraph.edgeConsensus.appendVector();
        for(const auto& q: results) {
            markerGraph.edgeConsensus.append(q);
        }
        markerGraph.edgeConsensusOverlappingBaseCount[edgeId] =
            (*assembleMarkerGraphEdgesData.threadEdgeConsensusOverlappingBaseCount[threadId])[i];

        if(storeCoverageData) {
            const auto& v = (*assembleMarkerGraphEdgesData.threadEdgeCoverageData[threadId])[i];
            markerGraph.edgeCoverageData.appendVector(v.begin(), v.end());
        }
    }


    // Remove the results computed by each thread.
    for(size_t threadId=0; threadId!=threadCount; threadId++) {
        assembleMarkerGraphEdgesData.threadEdgeIds[threadId]->remove();
        assembleMarkerGraphEdgesData.threadEdgeConsensus[threadId]->remove();
        assembleMarkerGraphEdgesData.threadEdgeConsensusOverlappingBaseCount[threadId]->remove();
        if(storeCoverageData) {
            assembleMarkerGraphEdgesData.threadEdgeCoverageData[threadId]->remove();
        }
    }
    assembleMarkerGraphEdgesData.threadEdgeIds.clear();
    assembleMarkerGraphEdgesData.threadEdgeConsensus.clear();
    assembleMarkerGraphEdgesData.threadEdgeConsensusOverlappingBaseCount.clear();
    if(storeCoverageData) {
        assembleMarkerGraphEdgesData.threadEdgeCoverageData.clear();
    }

    cout << timestamp << "assembleMarkerGraphEdges ends." << endl;
}



// Access coverage data for vertices and edges of the marker graph.
// This is only available if the run had Assembly.storeCoverageData set to True
// in sshasta.conf.
void Assembler::accessMarkerGraphCoverageData()
{
    try {
        markerGraph.vertexCoverageData.accessExistingReadOnly(
            largeDataName("MarkerGraphVerticesCoverageData"));
        markerGraph.edgeCoverageData.accessExistingReadOnly(
            largeDataName("MarkerGraphEdgesCoverageData"));

    } catch (const std::exception&) {
        throw runtime_error("Coverage data is not available. It is only stored if shasta.conf has "
            "Assembly.storeCoverageData set to True.");
    }
}



void Assembler::assembleMarkerGraphEdgesThreadFunction(size_t threadId)
{
    AssemblyGraph& assemblyGraph = *assemblyGraphPointer;
    const uint32_t markerGraphEdgeLengthThresholdForConsensus = assembleMarkerGraphEdgesData.markerGraphEdgeLengthThresholdForConsensus;
    const bool storeCoverageData = assembleMarkerGraphEdgesData.storeCoverageData;
    const bool assembleAllEdges = assembleMarkerGraphEdgesData.assembleAllEdges;

    // Allocate space for the results computed by this thread.
    assembleMarkerGraphEdgesData.threadEdgeIds[threadId] =
        make_shared< MemoryMapped::Vector<MarkerGraph::EdgeId> >();
    assembleMarkerGraphEdgesData.threadEdgeConsensus[threadId] =
        make_shared<MemoryMapped::VectorOfVectors<pair<Base, uint8_t>, uint64_t> >();
    assembleMarkerGraphEdgesData.threadEdgeConsensusOverlappingBaseCount[threadId] =
        make_shared< MemoryMapped::Vector<uint8_t> >();
    MemoryMapped::Vector<MarkerGraph::EdgeId>& edgeIds =
        *assembleMarkerGraphEdgesData.threadEdgeIds[threadId];
    MemoryMapped::VectorOfVectors<pair<Base, uint8_t>, uint64_t>& consensus =
        *assembleMarkerGraphEdgesData.threadEdgeConsensus[threadId];
    MemoryMapped::Vector<uint8_t>& overlappingBaseCountVector =
        *assembleMarkerGraphEdgesData.threadEdgeConsensusOverlappingBaseCount[threadId];

    edgeIds.createNew(
        largeDataName("tmp-assembleMarkerGraphEdges-edgeIds-" + to_string(threadId)), largeDataPageSize);
    consensus.createNew(
        largeDataName("tmp-assembleMarkerGraphEdges-consensus-" + to_string(threadId)), largeDataPageSize);
    overlappingBaseCountVector.createNew(
        largeDataName("tmp-assembleMarkerGraphEdges-consensus-overlappingBaseCount" + to_string(threadId)), largeDataPageSize);

    if(storeCoverageData) {
        assembleMarkerGraphEdgesData.threadEdgeCoverageData[threadId] =
            make_shared<MemoryMapped::VectorOfVectors<pair<uint32_t, CompressedCoverageData>, uint64_t> >();
        assembleMarkerGraphEdgesData.threadEdgeCoverageData[threadId]->createNew(
            largeDataName("tmp-assembleMarkerGraphEdges-edgeCoverageData" + to_string(threadId)), largeDataPageSize);
    }

    vector<Base> sequence;
    vector<uint32_t> repeatCounts;
    uint8_t overlappingBaseCount;
    vector< pair<uint32_t, CompressedCoverageData> > coverageData;

    const spoa::AlignmentType alignmentType = spoa::AlignmentType::kNW;
    const int8_t match = 1;
    const int8_t mismatch = -1;
    const int8_t gap = -1;
    auto spoaAlignmentEngine = spoa::createAlignmentEngine(alignmentType, match, mismatch, gap);
    auto spoaAlignmentGraph = spoa::createGraph();
    
    // Loop over batches assigned to this thread.
    uint64_t begin, end;
    while(getNextBatch(begin, end)) {
        if((begin % 10000000) == 0){
            std::lock_guard<std::mutex> lock(mutex);
            cout << timestamp << begin << "/" << markerGraph.edges.size() << endl;
        }

        // Loop over marker graph vertices assigned to this batch.
        for(MarkerGraph::EdgeId edgeId=begin; edgeId!=end; edgeId++) {

            // Figure out if we need to assemble this edge.
            bool shouldAssemble = true;
            if(not assembleAllEdges) {
                if(markerGraph.edges[edgeId].wasRemoved()) {
                    // The marker graph edge was removed.
                    shouldAssemble = false;
                } else {
                    // This marker graph edge was not removed.
                    // Check its assembly graph locations to see if it
                    // should be assembled.
                    shouldAssemble = false;
                    for(const auto& location: assemblyGraph.markerToAssemblyTable[edgeId]) {
                        const AssemblyGraph::EdgeId assemblyGraphEdgeId =
                            location.first;
                        if(assemblyGraph.isAssembledEdge(assemblyGraphEdgeId)) {
                            shouldAssemble = true;
                            break;
                        }
                    }
                }
            }

            // Compute the consensus, if necessary.
            if(!shouldAssemble) {
                markerGraph.edges[edgeId].wasAssembled = 0;
                sequence.clear();
                repeatCounts.clear();
                overlappingBaseCount = 0;
            } else {
                markerGraph.edges[edgeId].wasAssembled = 1;
                try {
                    ComputeMarkerGraphEdgeConsensusSequenceUsingSpoaDetail detail;
                    computeMarkerGraphEdgeConsensusSequenceUsingSpoa(
                        edgeId, markerGraphEdgeLengthThresholdForConsensus,
                        spoaAlignmentEngine, spoaAlignmentGraph,
                        sequence, repeatCounts, overlappingBaseCount,
                        detail,
                        storeCoverageData ? &coverageData : 0
                        );
                } catch(const std::exception& e) {
                    std::lock_guard<std::mutex> lock(mutex);
                    cout << "A standard exception was thrown while assembling "
                        "marker graph edge " << edgeId << ":" << endl;
                    cout << e.what() << endl;
                    throw;
                } catch(...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    cout << "A non-standard exception was thrown while assembling "
                        "marker graph edge " << edgeId << ":" << endl;
                    throw;
                }
            }

            // Store the results.
            edgeIds.push_back(edgeId);
            const size_t n = sequence.size();
            SHASTA_ASSERT(repeatCounts.size() == n);
            consensus.appendVector();
            for(size_t i=0; i<n; i++) {
                consensus.append(make_pair(sequence[i], repeatCounts[i]));
            }
            overlappingBaseCountVector.push_back(overlappingBaseCount);
            if(storeCoverageData) {
                assembleMarkerGraphEdgesData.threadEdgeCoverageData[threadId]->appendVector(coverageData);
            }

        }
    }
    
    edgeIds.unreserve();
    consensus.unreserve();
    overlappingBaseCountVector.unreserve();
}



void Assembler::accessMarkerGraphConsensus()
{
    markerGraph.vertexRepeatCounts.accessExistingReadOnly(
        largeDataName("MarkerGraphVertexRepeatCounts"));
    markerGraph.edgeConsensus.accessExistingReadOnly(
        largeDataName("MarkerGraphEdgesConsensus"));
    markerGraph.edgeConsensusOverlappingBaseCount.accessExistingReadOnly(
        largeDataName("MarkerGraphEdgesConsensusOverlappingBaseCount"));

}



// Create a coverage histogram for vertices and edges of the
// marker graph. This counts all vertices that are not isolated
// (are connected to no edges that are not marked removed)
// and all edges that are not marked as removed.
// Output is to csv files.
void Assembler::computeMarkerGraphCoverageHistogram()
{

    // Vertices.
    vector<uint64_t> vertexCoverageHistogram;
    for(MarkerGraph::VertexId vertexId=0;
        vertexId<markerGraph.vertexCount(); vertexId++) {

        // Check if this vertex is isolated.
        bool isIsolated = true;
        // Look at the out-edges.
        for(const MarkerGraph::EdgeId edgeId: markerGraph.edgesBySource[vertexId]) {
            if(!markerGraph.edges[edgeId].wasRemoved()) {
                isIsolated = false;
                break;
            }
        }
        if(isIsolated) {
            // We did not find any out-edges. Look at the in-edges.
            for(const MarkerGraph::EdgeId edgeId: markerGraph.edgesByTarget[vertexId]) {
                if(!markerGraph.edges[edgeId].wasRemoved()) {
                    isIsolated = false;
                    break;
                }
            }
        }

        // If isolated, skip it.
        if(isIsolated) {
            continue;
        }

        // Increment the histogram.
        const size_t coverage = markerGraph.vertexCoverage(vertexId);
        if(coverage >= vertexCoverageHistogram.size()) {
            vertexCoverageHistogram.resize(coverage+1, 0);
        }
        ++vertexCoverageHistogram[coverage];
    }
    ofstream verticesCsv("MarkerGraphVertexCoverageHistogram.csv");
    verticesCsv << "Coverage,Frequency\n";
    for(uint64_t coverage = 0; coverage<vertexCoverageHistogram.size(); coverage++) {
        const uint64_t frequency = vertexCoverageHistogram[coverage];
        verticesCsv << coverage << "," << frequency << "\n";
    }



    // Edges.
    vector<uint64_t> edgeCoverageHistogram;
    for(const MarkerGraph::Edge& edge: markerGraph.edges) {

        // If this edge was removed, skip it.
        if(edge.wasRemoved()) {
            continue;
        }

        // Increment the histogram.
        const size_t coverage = edge.coverage;
        if(coverage >= edgeCoverageHistogram.size()) {
            edgeCoverageHistogram.resize(coverage+1, 0);
        }
        ++edgeCoverageHistogram[coverage];
    }
    ofstream edgesCsv("MarkerGraphEdgeCoverageHistogram.csv");
    edgesCsv << "Coverage,Frequency\n";
    for(uint64_t coverage = 0; coverage<edgeCoverageHistogram.size(); coverage++) {
        const uint64_t frequency = edgeCoverageHistogram[coverage];
        edgesCsv << coverage << "," << frequency << "\n";
    }
}



void Assembler::removeMarkerGraphVertices()
{
    markerGraph.destructVertices();
    markerGraph.vertices().remove();
    markerGraph.vertexTable.remove();
}



// Set marker graph edge flags to specified values for all marker graph edges.
// Specify any value other than 0 or 1 leaves that flag unchanged.
// Only useful for debugging.
void Assembler::setMarkerGraphEdgeFlags(
    uint8_t wasRemovedByTransitiveReduction,
    uint8_t wasPruned,
    uint8_t isSuperBubbleEdge,
    uint8_t isLowCoverageCrossEdge,
    uint8_t wasAssembled)
{
    SHASTA_ASSERT(markerGraph.edges.isOpen);

    cout << timestamp << "Setting flags for all " <<
        markerGraph.edges.size() << " marker graph edges (2=unchanged):" << endl;
    cout << "wasRemovedByTransitiveReduction " << int(wasRemovedByTransitiveReduction) << endl;
    cout << "wasPruned " << int(wasPruned) << endl;
    cout << "isSuperBubbleEdge " << int(isSuperBubbleEdge) << endl;
    cout << "isLowCoverageCrossEdge  " << int(isLowCoverageCrossEdge) << endl;
    cout << "wasAssembled " << int(wasAssembled) << endl;

    for(MarkerGraph::Edge& edge: markerGraph.edges) {
        if(wasRemovedByTransitiveReduction == 0) {
            edge.wasRemovedByTransitiveReduction = 0;
        } else if(wasRemovedByTransitiveReduction == 1) {
            edge.wasRemovedByTransitiveReduction = 1;
        }
        if(wasPruned == 0) {
            edge.wasPruned = 0;
        } else if(wasPruned == 1) {
            edge.wasPruned = 1;
        }
        if(isSuperBubbleEdge == 0) {
            edge.isSuperBubbleEdge = 0;
        } else if(isSuperBubbleEdge == 1) {
            edge.isSuperBubbleEdge = 1;
        }
        if(isLowCoverageCrossEdge == 0) {
            edge.isLowCoverageCrossEdge = 0;
        } else if(isLowCoverageCrossEdge == 1) {
            edge.isLowCoverageCrossEdge = 1;
        }
        if(wasAssembled == 0) {
            edge.wasAssembled = 0;
        } else if(wasAssembled == 1) {
            edge.wasAssembled = 1;
        }
    }
    cout << timestamp << "Done." << endl;
}



// Each oriented read corresponds to a path in the marker graph.
// This function computes a subset of that path
// covering the specified range of marker ordinals for the given
// oriented read.
void Assembler::computeOrientedReadMarkerGraphPath(
    OrientedReadId orientedReadId,
    uint32_t firstOrdinal,
    uint32_t lastOrdinal,
    vector<MarkerGraph::EdgeId>& path,
    vector< pair<uint32_t, uint32_t> >& pathOrdinals
    ) const
{
    const uint64_t markerCount = markers.size(orientedReadId.getValue());
    SHASTA_ASSERT(lastOrdinal >= firstOrdinal);
    SHASTA_ASSERT(firstOrdinal < markerCount);
    SHASTA_ASSERT(lastOrdinal < markerCount);

    // Start with an empty path.
    path.clear();
    pathOrdinals.clear();

    // If too short, return an empty path.
    if(lastOrdinal == firstOrdinal) {
        return;
    }



    // Look for pairs (ordinal0, ordinal1) such that:
    // 1. Both ordinal0 and ordinal1 are in the specified ordinal range
    //    (>=firstOrdinal and <=lastOrdinal).
    // 2. Both ordinal0 and ordinal1 are associated with a marker graph vertex.
    // 3. Ordinal1 is the first ordinal greater than ordinal0
    //    associated with a marker graph vertex.
    // Because of 3, ordinal1>ordinal0.
    // Therefore, because of 1:
    //    firstOrdinal <= ordinal0 <  lastOrdinal
    //    firstOrdinal <  ordinal1 <= lastOrdinal
    // Once we have such a pair (ordinal0, ordinal1), we find the edge
    // the between the corresponding marker graph vertices, and add it
    // to the path.



    // Loop over possible values of ordinal0.
    for(uint32_t ordinal0=firstOrdinal; ordinal0<lastOrdinal; ordinal0++) {

        // Find the associated marker.
        const MarkerId markerId0 =  getMarkerId(orientedReadId, ordinal0);

        // Find the corresponding marker graph vertex.
        const MarkerGraph::CompressedVertexId compressedVertexId0 =
            markerGraph.vertexTable[markerId0];

        // If no associated marker graph vertex, skip.
        if(compressedVertexId0 == MarkerGraph::invalidCompressedVertexId) {
            continue;
        }
        const MarkerGraph::VertexId vertexId0 = compressedVertexId0;

        // Loop over possible values of ordinal1.
        for(uint32_t ordinal1=ordinal0+1; ordinal1<=lastOrdinal; ordinal1++) {

            // Find the associated marker.
            const MarkerId markerId1 =  getMarkerId(orientedReadId, ordinal1);

            // Find the corresponding marker graph vertex.
            const MarkerGraph::CompressedVertexId compressedVertexId1 =
                markerGraph.vertexTable[markerId1];

            // If no associated marker graph vertex, skip.
            if(compressedVertexId1 == MarkerGraph::invalidCompressedVertexId) {
                continue;
            }
            const MarkerGraph::VertexId vertexId1 = compressedVertexId1;

            // Locate the edge between these two vertices
            // and add it to the path.
            const span<const Uint40> outEdges0 = markerGraph.edgesBySource[vertexId0];
            bool found = false;
            for(MarkerGraph::EdgeId edgeId: outEdges0) {
                if(markerGraph.edges[edgeId].target == vertexId1) {
                    path.push_back(edgeId);
                    pathOrdinals.push_back(make_pair(ordinal0, ordinal1));
                    found = true;
                    break;
                }
            }
            if(not found) {
                // This is not good. We will assert below.
                // Write some details to facilitate debugging.
                std::lock_guard<std::mutex> lock(mutex);
                cout << "Could not locate marker graph edge for " << orientedReadId << endl;
                cout << "Number of markers on this read " << markerCount << endl;
                cout << "Between ordinals " << ordinal0 << " and " << ordinal1 << endl;
                cout << "vertexId0 " << vertexId0 << endl;
                cout << "vertexId1 " << vertexId1 << endl;
                cout << "Out-edges of vertexId0:" << endl;
                for(const Uint40 e: outEdges0) {
                    const MarkerGraph::EdgeId edgeId = e;
                    cout << "EdgeId " << edgeId << ", target vertex " << markerGraph.edges[edgeId].target << endl;
                }
                const span<const Uint40> inEdges1 = markerGraph.edgesByTarget[vertexId1];
                cout << "In-edges of vertexId1:" << endl;
                for(const Uint40 e: inEdges1) {
                    const MarkerGraph::EdgeId edgeId = e;
                    cout << "EdgeId " << edgeId << ", source vertex " << markerGraph.edges[edgeId].source << endl;
                }
                cout << "firstOrdinal " << firstOrdinal << endl;
                cout << "lastOrdinal " << lastOrdinal << endl;
                SHASTA_ASSERT(0);
            }
            break;
        }
    }
}



void Assembler::test()
{
    accessAllSoft();

    while(true) {
        cout << "Enter ReadId, strand, firstOrdinal, lastOrdinal:" << endl;
        ReadId readId;
        Strand strand;
        uint32_t firstOrdinal;
        uint32_t lastOrdinal;
        cin >> readId >> strand >> firstOrdinal >> lastOrdinal;

        vector<MarkerGraph::EdgeId> path;
        vector< pair<uint32_t, uint32_t> > pathOrdinals;
        computeOrientedReadMarkerGraphPath(
            OrientedReadId(readId, strand),
            firstOrdinal, lastOrdinal, path, pathOrdinals);

        cout << "Marker graph path: ";;
        copy(path.begin(), path.end(), ostream_iterator<MarkerGraph::EdgeId>(cout, " "));
        cout << endl;
    }
}



// Given a marker graph vertex, follow all of the contributing oriented
// reads to their next vertex, but without moving forward more than
// maxSkip markers.
// In the returned vector, each entry correspond to a marker in the given vertex
// (in the same order) and gives the next VertexId for that oriented read.
// The next VertexId can be invalidVertexId if the oriented read has no vertices
// past the starting VertexId.
void Assembler::findNextMarkerGraphVertices(
    MarkerGraph::VertexId vertexId,
    uint32_t maxSkip,
    vector<MarkerGraph::VertexId>& nextVertices) const
{
    nextVertices.clear();
    const span<const MarkerId> markerIds = markerGraph.getVertexMarkerIds(vertexId);
    for(const MarkerId markerId: markerIds) {
        OrientedReadId orientedReadId;
        uint32_t ordinal;
        tie(orientedReadId, ordinal) = findMarkerId(markerId);
        const uint32_t markerCount = uint32_t(markers.size(orientedReadId.getValue()));
        MarkerGraph::VertexId nextVertexId = MarkerGraph::invalidVertexId;
        uint32_t ordinalEnd = min(markerCount, ordinal + maxSkip + 1);
        for(++ordinal; ordinal<ordinalEnd; ++ordinal) {
            const MarkerId nextMarkerId = getMarkerId(orientedReadId, ordinal);
            const MarkerGraph::CompressedVertexId compressedNextVertexId = markerGraph.vertexTable[nextMarkerId];
            if(compressedNextVertexId != MarkerGraph::invalidCompressedVertexId) {
                nextVertexId = MarkerGraph::VertexId(compressedNextVertexId);
                break;
            }
        }
        nextVertices.push_back(nextVertexId);
    }

}



void Assembler::debugWriteMarkerGraph(const string& fileNamePrefix) const
{
    using VertexId = MarkerGraph::VertexId;
    using EdgeId = MarkerGraph::EdgeId;



    // Vertices.
    if(markerGraph.vertices().isOpen()) {
        ofstream csv(fileNamePrefix + "MarkerGraphVertices.csv");
        csv << "VertexId,MarkerId,OrientedReadId,Ordinal,\n";

        for(VertexId vertexId=0; vertexId<markerGraph.vertexCount(); vertexId++) {
            const span<const MarkerId> markerIds = markerGraph.getVertexMarkerIds(vertexId);
            for(const MarkerId markerId: markerIds) {
                OrientedReadId orientedReadId;
                uint32_t ordinal;
                tie(orientedReadId, ordinal) = findMarkerId(markerId);

                csv << vertexId << ",";
                csv << markerId << ",";
                csv << orientedReadId << ",";
                csv << ordinal << ",";
                csv << "\n";
            }
        }
    }



    // Reverse complement vertices.
    if(markerGraph.reverseComplementVertex.isOpen) {
        ofstream csv(fileNamePrefix + "MarkerGraphReverseComplementVertices.csv");
        csv << "VertexId,VertexIdRc,\n";

        for(VertexId vertexId=0; vertexId<markerGraph.vertexCount(); vertexId++) {
            csv << vertexId << ",";
            csv << markerGraph.reverseComplementVertex[vertexId] << ",";
            csv << "\n";
        }
    }



    // Edges.
    if(markerGraph.edges.isOpen) {
        ofstream csv(fileNamePrefix + "MarkerGraphEdges.csv");
        csv << "EdgeId,Source,Target,\n";

        for(EdgeId edgeId=0; edgeId<markerGraph.edges.size(); edgeId++) {
            const MarkerGraph::Edge& edge = markerGraph.edges[edgeId];

            csv << edgeId << ",";
            csv << edge.source << ",";
            csv << edge.target << ",";
            csv << "\n";
        }
    }



    // Edges by source.
    if(markerGraph.edgesBySource.isOpen()) {
        ofstream csv(fileNamePrefix + "MarkerGraphEdgesBySource.csv");
        csv << "Source,Target0,Target1,Target2,\n";

        for(VertexId vertexId=0; vertexId<markerGraph.edgesBySource.size(); vertexId++) {

            csv << vertexId << ",";
            for(const auto& edgeId: markerGraph.edgesBySource[vertexId]) {
                csv << edgeId << ",";
            }
            csv << "\n";
        }
    }
}



// Assemble the RLE sequence of a path of the marker graph, under the assumption
// that, for each edge, all oriented reads have exactly the same sequence.
// This will be the case if edges were created by Assembler::createMarkerGraphEdgesStrict.
void Assembler::assembleMarkerGraphPathRleStrict(
    span<const MarkerGraph::EdgeId> path,
    vector<Base>& rleSequence
    ) const
{
    using VertexId = MarkerGraph::VertexId;
    using EdgeId = MarkerGraph::EdgeId;
    const uint64_t k = assemblerInfo->k;

    // Start with no sequence.
    rleSequence.clear();
    if(path.empty()) {
        return;
    }

    // Add the RLE sequence of the first vertex.
    VertexId v0 = markerGraph.edges[path.front()].source;
    const MarkerId firstMarkerId = markerGraph.getVertexMarkerIds(v0)[0];
    const CompressedMarker& firstMarker = markers.begin()[firstMarkerId];
    const KmerId kmerId = firstMarker.kmerId;
    const Kmer kmer(kmerId, k);
    for(uint64_t i=0; i<k; i++) {
        rleSequence.push_back(kmer[i]);
    }



    // Loop over edges of the path.
    for(const EdgeId edgeId: path) {
        const MarkerGraph::Edge& edge = markerGraph.edges[edgeId];
        SHASTA_ASSERT(edge.source == v0);
        const VertexId v1 = edge.target;

        const span<const MarkerInterval> markerIntervals = markerGraph.edgeMarkerIntervals[edgeId];
        SHASTA_ASSERT(not markerIntervals.empty());

        // Get the RLE sequence and check that all the MarkerIntervals agree.
        // This will be the case if edges were created by Assembler::createMarkerGraphEdgesStrict.
        uint64_t overlappingRleBaseCount;
        vector<Base> edgeRleSequence;
        getMarkerIntervalRleSequence(
            markerIntervals.front(),
            overlappingRleBaseCount,
            edgeRleSequence);
        uint64_t markerIntervalOverlappingRleBaseCount;
        vector<Base> markerIntervalRleSequence;
        for(const MarkerInterval& markerInterval: markerIntervals) {
            getMarkerIntervalRleSequence(
                markerInterval,
                markerIntervalOverlappingRleBaseCount,
                markerIntervalRleSequence);
            SHASTA_ASSERT(markerIntervalOverlappingRleBaseCount == overlappingRleBaseCount);
            SHASTA_ASSERT(markerIntervalRleSequence == edgeRleSequence);
        }



        // Construct the sequence of the v1 vertex.
        const MarkerId markerId1 = markerGraph.getVertexMarkerIds(v1)[0];
        const CompressedMarker& marker1 = markers.begin()[markerId1];
        const KmerId kmerId1 = marker1.kmerId;
        const Kmer kmer1(kmerId1, k);


        // Add the sequence of this edge and the v1 vertex.
        if(overlappingRleBaseCount == 0) {

            // There is no overlap.

            // Add the edge sequence.
            copy(edgeRleSequence.begin(), edgeRleSequence.end(), back_inserter(rleSequence));

            // Add the entire sequence of v1.
            for(uint64_t i=0; i<k; i++) {
                rleSequence.push_back(kmer1[i]);
            }

        } else {

            // There is overlap.
            // Add the sequence of v1, excluding the overlapping bases.
            for(uint64_t i=overlappingRleBaseCount; i<k; i++) {
                rleSequence.push_back(kmer1[i]);
            }
        }


        // Prepare to process the next edge.
        v0 = v1;
    }
}



void Assembler::assembleAssemblyGraphEdgeRleStrict(
    AssemblyGraph::EdgeId edgeId,
    vector<Base>& rleSequence
) const
{
    const AssemblyGraph& assemblyGraph = *assemblyGraphPointer;
    assembleMarkerGraphPathRleStrict(
        assemblyGraph.edgeLists[edgeId],
        rleSequence);
}



// Get the RLE sequence implied by a MarkerInterval.
// If the markers overlap, returns the number of
// overlapping RLE bases in overlappingRleBaseCount
// and empty rleSequence.
// Otherwise, returns zero overlappingRleBaseCount
// and the intervening sequence in rleSequence
// (which can be empty if the two markers are exactly adjacent).
void Assembler::getMarkerIntervalRleSequence(
    const MarkerInterval& markerInterval,
    uint64_t& overlappingRleBaseCount,
    vector<Base>& rleSequence) const
{
    const uint64_t k = assemblerInfo->k;
    const OrientedReadId orientedReadId = markerInterval.orientedReadId;

    // Extract the k-mers and their RLE positions in this oriented read.
    array<Kmer, 2> kmers;
    array<uint32_t, 2> positions;
    for(uint64_t i=0; i<2; i++) {
        const MarkerId markerId = getMarkerId(orientedReadId, markerInterval.ordinals[i]);
        const CompressedMarker& compressedMarker = markers.begin()[markerId];
        kmers[i] = Kmer(compressedMarker.kmerId, k);
        positions[i] = compressedMarker.position;
    }


    if(positions[1] < positions[0] + k) {
        // The two markers overlap.
        overlappingRleBaseCount = (positions[0] + k) - positions[1];
        rleSequence.clear();
    } else {
        // The two markers don't overlap.
        overlappingRleBaseCount = 0;
        rleSequence.clear();
        for(uint32_t position=positions[0]+uint32_t(k); position<positions[1]; position++) {
            rleSequence.push_back(getReads().getOrientedReadBase(orientedReadId, position));
        }
    }
}

