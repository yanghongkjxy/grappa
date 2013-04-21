/*
 * Example of:
 * (partial) triangle listing
 *
 * A(x,z) :- R(x,y), R(y,z), R(z,x)
 */


// util includes
#include "Tuple.hpp"
#include "relation_IO.hpp"

// graph500/
#include "../graph500/generator/make_graph.h"
#include "../graph500/generator/utils.h"
#include "../graph500/grappa/timer.h"
#include "../graph500/grappa/common.h"
#include "../graph500/grappa/oned_csr.h" // just for other graph gen stuff besides tuple->csr
#include "../graph500/prng.h"


// Grappa includes
#include <Grappa.hpp>
#include "MatchesDHT.hpp"
#include <Cache.hpp>
#include <ParallelLoop.hpp>
#include <GlobalCompletionEvent.hpp>
#include <AsyncDelegate.hpp>

// command line parameters

// file input
DEFINE_string( in, "", "Input file relation" );
DEFINE_uint64( fileNumTuples, 0, "Number of lines in file" );

// generating input data
DEFINE_uint64( scale, 7, "Log of number of vertices" );
DEFINE_uint64( edgefactor, 16, "Median degree to try to generate" );
DEFINE_bool( undirected, false, "Generated graph implies undirected edges" );

DEFINE_bool( print, false, "Print results" );

using namespace Grappa;

std::ostream& operator<<( std::ostream& o, const Tuple& t ) {
  o << "T( ";
  for (uint64_t i=0; i<TUPLE_LEN; i++) {
    o << t.columns[i] << ", ";
  }
  o << ")";
}

typedef uint64_t Column;

uint64_t identity_hash( int64_t k ) {
  return k;
}

// local portal to DHT
typedef MatchesDHT<int64_t, Tuple, identity_hash> DHT_type;
DHT_type joinTable;

// local RO copies
GlobalAddress<Tuple> local_tuples;
Column local_join1Left, local_join1Right, local_join2Right;
Column local_select;
GlobalAddress<Tuple> IndexBase;

// local counters
uint64_t local_triangle_count;


template< typename T >
inline void print_array(const char * name, GlobalAddress<T> base, size_t nelem, int width = 10) {
  std::stringstream ss; ss << name << ": [";
  for (size_t i=0; i<nelem; i++) {
    if (i % width == 0) ss << "\n";
    ss << " " << delegate::read(base+i);
  }
  ss << " ]"; VLOG(1) << ss.str();
}


GlobalAddress<Tuple> generate_data( size_t scale, size_t edgefactor, size_t * num ) {
  // initialize rng 
  //init_random(); 
  //userseed = 10;
  uint64_t N = (1L<<scale);
  uint64_t desired_nedges = edgefactor*N;

  VLOG(1) << "Generating edges (desired " << desired_nedges << ")";
  tuple_graph tg;
  make_graph( scale, desired_nedges, userseed, userseed, &tg.nedge, &tg.edges );
  VLOG(1) << "edge count: " << tg.nedge;

  size_t nedge = FLAGS_undirected ? 2*tg.nedge : tg.nedge;

  // allocate the tuples
  GlobalAddress<Tuple> base = Grappa_typed_malloc<Tuple>( nedge );

  // copy and transform from edge representation to Tuple representation
  forall_localized(tg.edges, tg.nedge, [base](int64_t start, int64_t n, packed_edge * first) {
    // FIXME: I know write_async messages are one globaladdress 
    // and one tuple, but make it encapsulated
    int64_t num_messages =  FLAGS_undirected ? 2*n : n;

    char msg_buf[num_messages * sizeof(Message<std::function<void(GlobalAddress<Tuple>, Tuple)>>)];
    MessagePool write_pool(msg_buf, sizeof(msg_buf));
    for (int64_t i=0; i<n; i++) {
      auto e = first[i];

      Tuple t;
      t.columns[0] = get_v0_from_edge( &e );
      t.columns[1] = get_v1_from_edge( &e );

      if ( FLAGS_undirected ) {
        delegate::write_async( write_pool, base+start+2*i, t ); 
        t.columns[0] = get_v1_from_edge( &e );
        t.columns[1] = get_v0_from_edge( &e );
        delegate::write_async( write_pool, base+start+2*i+1, t ); 
      } else {
        delegate::write_async( write_pool, base+start+i, t ); 
      }
      // optimally I'd like async WO cache op since this will coalesce the write as well
     }
   });

  *num = nedge;

  print_array( "generated tuples", base, nedge, 1 );

  // TODO: remove self-edges and duplicates (e.g. prefix sum and compaction)

  return base;
}

void scanAndHash( GlobalAddress<Tuple> tuples, size_t num ) {
  forall_localized( tuples, num, [](int64_t i, Tuple& t) {
    int64_t key = t.columns[local_join1Left];

    VLOG(2) << "insert " << key << " | " << t;
    joinTable.insert( key, t );
  });
}


void triangles( GlobalAddress<Tuple> tuples, size_t num_tuples, Column ji1, Column ji2, Column ji3, Column s ) {
  // initialization
  on_all_cores( [tuples, ji1, ji2, ji3, s] {
    local_tuples = tuples;
    local_join1Left = ji1;
    local_join1Right = ji2;
    local_join2Right = ji3;
    local_select = s;
    local_triangle_count = 0;
  });
    
  // scan tuples and hash join col 1
  VLOG(1) << "Scan tuples, creating index on subject";
  
  double start, end;
  start = Grappa_walltime();
  {
    scanAndHash( tuples, num_tuples );
  } 
  end = Grappa_walltime();
  
  VLOG(1) << "insertions: " << (end-start)/num_tuples << " per sec";

#if DEBUG
  printAll(tuples, num_tuples);
#endif

  // tell the DHT we are done with inserts
  VLOG(1) << "DHT setting to RO";
#if SORTED_KEYS
  GlobalAddress<Tuple> index_base = DHT_type::set_RO_global( &joinTable );
  on_all_cores([index_base] {
      IndexBase = index_base;
  });
#else
  DHT_type::set_RO_global( &joinTable );
#endif

  start = Grappa_walltime();
  VLOG(1) << "Starting 1st join";
  forall_localized( tuples, num_tuples, [](int64_t i, Tuple& t) {
    int64_t key = t.columns[local_join1Right];
   
    // will pass on this first vertex to compare in the select 
    int64_t x1 = t.columns[local_join1Left];

#if SORTED_KEYS
    // first join
    uint64_t results_idx;
    size_t num_results = joinTable.lookup( key, &results_idx );
    DVLOG(4) << "key " << t << " finds (" << results_idx << ", " << num_results << ")";
   
    // iterate over the first join results in parallel
    // (iterations must spawn with synch object `local_gce`)
    forall_here_async_public( results_idx, num_results, [x1](int64_t start, int64_t iters) {
      Tuple subset_stor[iters];
      Incoherent<Tuple>::RO subset( IndexBase+start, iters, &subset_stor );
#else // MATCHES_DHT
    // first join
    GlobalAddress<Tuple> results_addr;
    size_t num_results = joinTable.lookup( key, &results_addr );
    
    // iterate over the first join results in parallel
    // (iterations must spawn with synch object `local_gce`)
    /* not yet supported: forall_here_async_public< GCE=&local_gce >( 0, num_results, [x1,results_addr](int64_t start, int64_t iters) { */
    forall_here_async( 0, num_results, [x1,results_addr](int64_t start, int64_t iters) { 
      Tuple subset_stor[iters];
      Incoherent<Tuple>::RO subset( results_addr+start, iters, subset_stor );
#endif

      for ( int64_t i=0; i<iters; i++ ) {

        int64_t key = subset[i].columns[local_join2Right];

        int64_t x2 = subset[i].columns[0];
        
        if ( !(x1 < x2) ) {  // early select on ordering
          continue;
        }

#if SORTED_KEYS
        // second join
        uint64_t results_idx;
        size_t num_results = joinTable.lookup( key, &results_idx );

        VLOG(5) << "results key " << key << " (n=" << num_results;
        
        // iterate over the second join results in parallel
        // (iterations must spawn with synch object `local_gce`)
        forall_here_async_public( results_idx, num_results, [x1](int64_t start, int64_t iters) {
          Tuple subset_stor[iters];
          Incoherent<Tuple>::RO subset( IndexBase+start, iters, subset_stor );
#else // MATCHES_DHT
        // second join
        GlobalAddress<Tuple> results_addr;
        size_t num_results = joinTable.lookup( key, &results_addr );

        VLOG(5) << "results key " << key << " (n=" << num_results;
        
        // iterate over the second join results in parallel
        // (iterations must spawn with synch object `local_gce`)
        /* not yet supported: forall_here_async_public< GCE=&local_gce >( 0, num_results, [x1,results_addr](int64_t start, int64_t iters) { */
        forall_here_async( 0, num_results, [x1,x2,results_addr](int64_t start, int64_t iters) {
          Tuple subset_stor[iters];
          Incoherent<Tuple>::RO subset( results_addr+start, iters, subset_stor );
#endif
          
          for ( int64_t i=0; i<iters; i++ ) {
            int64_t r = subset[i].columns[0];
            if ( x2 < r ) {  // select on ordering 
              if ( subset[i].columns[local_select] == x1 ) { // select on triangle
                if (FLAGS_print) {
                  VLOG(1) << x1 << " " << x2 << " " << r;
                }
                local_triangle_count++;
              }
            }
          }
        }); // end loop over 2nd join results
      }
    }); // end loop over 1st join results
  }); // end outer loop over tuples
         
  
  uint64_t total_triangle_count = Grappa::reduce<uint64_t, collective_add>( &local_triangle_count ); 


      end = Grappa_walltime();
  VLOG(1) << "joins: " << (end-start) << " seconds; total_triangle_count=" << total_triangle_count;
}

void user_main( int * ignore ) {

  GlobalAddress<Tuple> tuples;
  size_t num_tuples;

  if ( FLAGS_in == "" ) {
    VLOG(1) << "Generating some data";
    tuples = generate_data( FLAGS_scale, FLAGS_edgefactor, &num_tuples );
  } else {
    VLOG(1) << "Reading data from " << FLAGS_in;
    
    tuples = Grappa_typed_malloc<Tuple>( FLAGS_fileNumTuples );
    readTuples( FLAGS_in, tuples, FLAGS_fileNumTuples );
    num_tuples = FLAGS_fileNumTuples;
  }

  DHT_type::init_global_DHT( &joinTable, 64 );

  Column joinIndex1 = 0; // subject
  Column joinIndex2 = 1; // object
  Column select = 1; // object

  // triangle (assume one index to build)
  triangles( tuples, num_tuples, joinIndex1, joinIndex2, joinIndex2, select ); 
}


/// Main() entry
int main (int argc, char** argv) {
    Grappa_init( &argc, &argv ); 
    Grappa_activate();

    Grappa_run_user_main( &user_main, (int*)NULL );
    CHECK( Grappa_done() == true ) << "Grappa not done before scheduler exit";
    Grappa_finish( 0 );
}



// insert conflicts use java-style arraylist, enabling memcpy for next step of join