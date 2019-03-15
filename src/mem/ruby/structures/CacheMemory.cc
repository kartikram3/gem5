/*
 * Copyright (c) 1999-2012 Mark D. Hill and David A. Wood
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/structures/CacheMemory.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "debug/RubyCache.hh"
#include "debug/RubyCacheTrace.hh"
#include "debug/RubyResourceStalls.hh"
#include "debug/RubyStats.hh"
#include "mem/protocol/AccessPermission.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/system/WeightedLRUPolicy.hh"

using namespace std;

ostream&
operator<<(ostream& out, const CacheMemory& obj)
{
    obj.print(out);
    out << flush;
    return out;
}

CacheMemory *
RubyCacheParams::create()
{
    return new CacheMemory(this);
}

CacheMemory::CacheMemory(const Params *p)
    : SimObject(p),
    dataArray(p->dataArrayBanks, p->dataAccessLatency,
              p->start_index_bit, p->ruby_system),
    tagArray(p->tagArrayBanks, p->tagAccessLatency,
             p->start_index_bit, p->ruby_system)
{
    m_cache_size = p->size;
    m_cache_assoc = p->assoc;
    m_replacementPolicy_ptr = p->replacement_policy;
    m_replacementPolicy_ptr->setCache(this);
    m_start_index_bit = p->start_index_bit;
    m_is_instruction_only_cache = p->is_icache;
    m_resource_stalls = p->resourceStalls;
    m_block_size = p->block_size;  // may be 0 at this point. Updated in init()

    repl_id = 0;
    buf_size = 16;
    order = 0;

    for (int i=0; i<buf_size; i++){
      miss_buffer.push_back({NULL, 1});
    }

    buffer_full=false;
}

void
CacheMemory::init()
{
    if (m_block_size == 0) {
        m_block_size = RubySystem::getBlockSizeBytes();
    }
    m_cache_num_sets = (m_cache_size / m_cache_assoc) / m_block_size;
    assert(m_cache_num_sets > 1);
    m_cache_num_set_bits = floorLog2(m_cache_num_sets);
    assert(m_cache_num_set_bits > 0);

    m_cache.resize(m_cache_num_sets,
                    std::vector<AbstractCacheEntry*>(m_cache_assoc, nullptr));
}

void
CacheMemory::updateTagIndex(Addr buffer_addr, Addr set_addr, int way){

   assert(m_tag_index.find(buffer_addr) == m_tag_index.end());
   assert(m_tag_index_time.find(buffer_addr) == m_tag_index_time.end());
   assert(m_tag_index.find(set_addr) != m_tag_index.end());
   assert(m_tag_index_time.find(set_addr) != m_tag_index_time.end());

   m_tag_index.erase(m_tag_index.find(set_addr));
   m_tag_index_time.erase(m_tag_index_time.find(set_addr));
   m_tag_index[buffer_addr]=way;
   m_tag_index_time[buffer_addr]=curTick();

   return;
}

int
CacheMemory::getLatest(int cacheSet){
   //get the latest valid cache line in the set
   Addr temp;
   uint64_t time = 0;
   int select = -1;

   for (int i=0; i<m_cache_assoc; i++){
       if (m_cache[cacheSet][i]){
           temp = m_cache[cacheSet][i]->m_Address;
           assert(m_tag_index_time.find(temp) !=
                  m_tag_index_time.end());
           if (m_tag_index_time[temp] > time){
              time = m_tag_index_time[temp];
              select = i;
           }
       }
   }

   assert(select != -1);
   return select;
}

bool
CacheMemory::lastAccessTime(Addr address){
  int loc = findTagInSet(addressToCacheSet(address), address);
  uint64_t _time = -1;
  if (loc != -1){
     if (loc < 100){
        assert(m_tag_index_time.find(address)
               != m_tag_index_time.end());
        _time = m_tag_index_time[address];
        return ((_time - curTick()) < 100000)
     } else {
        assert(miss_buffer[loc-100].e);
        _time = miss_buffer[loc-100].age;
        return ((_time - curTick()) < 100000)
     }
  }
  panic("Uh oh, we are checking access time incorrectly\n");
}

bool
CacheMemory::checkEtoS(Addr address){
  //check the buffer for the address
  int loc = findTagInSet(addressToCacheSet(address), address);
  if (loc != -1){
      if (loc < 100){
          assert(m_tag_index_EtoS.find(address)
                 != m_tag_index_EtoS.end());
          return
            ((curTick() - m_tag_index_EtoS[address].time)
             < 100000);
      }else{
          assert(miss_buffer[loc-100].e);
          return(miss_buffer[loc-100].EtoS;
      }
  }
  panic("Uh oh, we are checking EtoS incorrectly\n");
}

int
CacheMemory::getTransitionCode(Addr address){
   //if recent change, then the cache lines need to be
   //loaded not too much later
   uint64_t curTime = curTick();
   bool isMiss = lastAccessTime(address);
   bool EtoS = checkEtoS(address);

   if ((!EtoS) && isMiss) return 1;
   else if (EtoS && isMiss) return 2;
   else return 0;
}

//squash the side effects associated with the cache accesses
//switch the kicked out entries
void
CacheMemory::squashSideEffect(){
   assert(buffer_full);
   uint64_t time=curTick();
   int cacheSet=0;
   int way=0;
   Addr buffer_addr,set_addr;
   AbstractCacheEntry *temp_e = NULL;
   //switch the positions of the two entries
   for (int i=0; i<buf_size; i++){
      //switch the recent entries (within the last 100 cycles);
      if (miss_buffer[i].e) {
         if ((time - miss_buffer[i].age) < 100000){
             //put it back into cache and exchange
             //with the most recent entries
             temp_e = miss_buffer[i].e;
             cacheSet =
               addressToCacheSet(miss_buffer[i].e->m_Address);
             way =
               getLatest(cacheSet);
             buffer_addr = miss_buffer[i].e->m_Address;
             set_addr = m_cache[cacheSet][way]->m_Address;
             miss_buffer[i].e = m_cache[cacheSet][way];
             m_cache[cacheSet][way] = temp_e;
             updateTagIndex(buffer_addr,set_addr,way);
         }
      }
   }
}

CacheMemory::~CacheMemory()
{
    if (m_replacementPolicy_ptr)
        delete m_replacementPolicy_ptr;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            delete m_cache[i][j];
        }
    }
}

// convert a Address to its location in the cache
int64_t
CacheMemory::addressToCacheSet(Addr address) const
{
    assert(address == makeLineAddress(address));
    return bitSelect(address, m_start_index_bit,
                     m_start_index_bit + m_cache_num_set_bits - 1);
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSet(int64_t cacheSet, Addr tag) const
{
    assert(tag == makeLineAddress(tag));

    //search the buffer for the cache lines
    //this can be helpful to find the correct
    //solution
    for (int i=0; i<buf_size; i++){
        if (miss_buffer[i].e)
          if (miss_buffer[i].e->m_Address == tag ){
            assert(m_tag_index.find(tag) == m_tag_index.end());
            assert(miss_buffer[i].e->m_Permission !=
                AccessPermission_NotPresent);
            return (100+i);
          }
    }

    // search the set for the tags
    auto it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        if (m_cache[cacheSet][it->second]->m_Permission !=
            AccessPermission_NotPresent)
            return it->second;


    return -1; // Not found
}

// Given a cache index: returns the index of the tag in a set.
// returns -1 if the tag is not found.
int
CacheMemory::findTagInSetIgnorePermissions(int64_t cacheSet,
                                           Addr tag) const
{
    panic("We should not use function findTagInSetIgnorePermissions");
    assert(tag == makeLineAddress(tag));
    // search the set for the tags
    auto it = m_tag_index.find(tag);
    if (it != m_tag_index.end())
        return it->second;
    return -1; // Not found
}

// Given an unique cache block identifier (idx): return the valid address
// stored by the cache block.  If the block is invalid/notpresent, the
// function returns the 0 address
Addr
CacheMemory::getAddressAtIdx(int idx) const
{
    panic("We should not use the function getAddressAtIdx\n");
    Addr tmp(0);

    int set = idx / m_cache_assoc;
    assert(set < m_cache_num_sets);

    int way = idx - set * m_cache_assoc;
    assert (way < m_cache_assoc);

    AbstractCacheEntry* entry = m_cache[set][way];
    if (entry == NULL ||
        entry->m_Permission == AccessPermission_Invalid ||
        entry->m_Permission == AccessPermission_NotPresent) {
        return tmp;
    }
    return entry->m_Address;
}

bool
CacheMemory::tryCacheAccess(Addr address, RubyRequestType type,
                            DataBlock*& data_ptr)
{
    panic("We should not used function tryCacheAccess\n");
    assert(address == makeLineAddress(address));
    DPRINTF(RubyCache, "address: %#x\n", address);
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc != -1) {
        // Do we even have a tag match?
        AbstractCacheEntry* entry = m_cache[cacheSet][loc];
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
        data_ptr = &(entry->getDataBlk());

        if (entry->m_Permission == AccessPermission_Read_Write) {
            return true;
        }
        if ((entry->m_Permission == AccessPermission_Read_Only) &&
            (type == RubyRequestType_LD || type == RubyRequestType_IFETCH)) {
            return true;
        }
        // The line must not be accessible
    }
    data_ptr = NULL;
    return false;
}

bool
CacheMemory::testCacheAccess(Addr address, RubyRequestType type,
                             DataBlock*& data_ptr)
{
    panic("We should not used function testCacheAccess\n");
    assert(address == makeLineAddress(address));
    DPRINTF(RubyCache, "address: %#x\n", address);
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc != -1) {
        // Do we even have a tag match?
        AbstractCacheEntry* entry = m_cache[cacheSet][loc];
        m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
        data_ptr = &(entry->getDataBlk());

        return m_cache[cacheSet][loc]->m_Permission !=
            AccessPermission_NotPresent;
    }

    data_ptr = NULL;
    return false;
}

//update the buffer stats if the recent hit was
//to the cache line
void
CacheMemory::updateBufStats(Addr address)
{
   //this is used to update the buf stats
   for (int i=0; i<buf_size; i++){
     if (miss_buffer[i].e){
        if (miss_buffer[i].e->m_Address == address){
           m_miss_buf_hits++;
        }
     }
   }
}

//tests to see if an address is present in the cache
bool
CacheMemory::isTagPresent(Addr address) const
{
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc == -1) {
        // We didn't find the tag
        DPRINTF(RubyCache, "No tag match for address: %#x\n", address);
        return false;
    }

    if (loc >= 100){  //means we found a match in the miss buffer
       //no need to do anything extra though
    }

    DPRINTF(RubyCache, "address: %#x found\n", address);
    return true;
}

//check that there aren't any duplicates
//in the cache ...
void
CacheMemory::checkDuplicates()
{
   //check that there aren't any duplicates
   for (int i=0; i<buf_size; i++){
     for (int j=i+1; j<buf_size; j++){
         if ((miss_buffer[i].e != NULL) &&
             (miss_buffer[j].e != NULL)){
             assert(miss_buffer[i].e!=miss_buffer[j].e);
         }
     }
   }
}


bool
CacheMemory::cacheAvailIcache(Addr address){
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);

    //next check the sets and see if there is any room there
    for (int i = 0; i < m_cache_assoc; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry != NULL) {
            if (entry->m_Address == address ||
                entry->m_Permission == AccessPermission_NotPresent) {
                return true;
            }
        } else {
            return true;
        }
    }

    return false;
}


// Returns true if there is:
//   a) a tag match on this address or there is
//   b) an unused line in the same cache "way"
bool
CacheMemory::cacheAvail(Addr address)
{
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);

    //first check the buffer ... see if it is already there
    for (int i=0; i<buf_size ; i++) {
        AbstractCacheEntry* entry = miss_buffer[i].e;
        if (entry != NULL) {
            if (entry->m_Address == address) {
                return true;
            }
        }
    }


    //next check the sets and see if there is any room there
    for (int i = 0; i < m_cache_assoc; i++) {
        AbstractCacheEntry* entry = m_cache[cacheSet][i];
        if (entry != NULL) {
            if (entry->m_Address == address ||
                entry->m_Permission == AccessPermission_NotPresent) {
                // Already in the cache or we found an empty entry
                if (address == 0x5d440){
                  fprintf(stderr," We found 0x5d440 in the cache\n ");
                }
                return true;
            }
        } else {
             if (address == 0x5d440){
               fprintf(stderr," We found empty entry for 0x5d440\n ");
             }
            return true;
        }
    }

    bool nullAvailable = false;
    int nullIdx = 0;
    for (int i=0; i<buf_size ; i++) {
        AbstractCacheEntry* entry = miss_buffer[i].e;
        if (entry != NULL) {
            if (entry->m_Address == address) {
                //Already in the cache or we found an empty entry
                return true;
            }
        } else {
            nullAvailable = true;
            nullIdx = i;
        }
    }

    //if buffer room available, shift it there (based on the repl policy)
    //this happens only in the start ... should not happen later on
    Addr mov_addr = m_cache[cacheSet][0]->m_Address;
    if (!buffer_full){
       if (nullAvailable){
          miss_buffer[nullIdx].e = m_cache[cacheSet][0];
          miss_buffer[nullIdx].age = curTick();
          assert(m_cache[cacheSet][0]->m_Permission !=
              AccessPermission_NotPresent);
          m_cache[cacheSet][0] = NULL;
          assert(m_tag_index.find(mov_addr) != m_tag_index.end());
          m_tag_index.erase(m_tag_index.find(mov_addr));
          m_tag_index_time.erase(mov_addr);
          //fprintf(stderr, "Erased addr is %lx \n", mov_addr);
          checkDuplicates();
          return true;
       } else {
          buffer_full = true;
       }
    }

    //if the buffer is full, we are done


    return false;
}


AbstractCacheEntry*
CacheMemory::allocateIcache(Addr address,
    AbstractCacheEntry *entry, bool touch)
{
    assert(address == makeLineAddress(address));
    assert(!isTagPresent(address));
    assert(cacheAvailIcache(address));
    DPRINTF(RubyCache, "address: %#x\n", address);

    if (address == 0x5d440){
      fprintf(stderr," We select 0x5d440 for allocate\n ");
    }

    // Find the first open slot
    int64_t cacheSet = addressToCacheSet(address);
    std::vector<AbstractCacheEntry*> &set = m_cache[cacheSet];
    for (int i = 0; i < m_cache_assoc; i++) {
        if (!set[i] || set[i]->m_Permission == AccessPermission_NotPresent) {
          if (set[i] && (set[i] != entry)) {
              warn_once("This protocol contains a cache entry handling bug: "
                  "Entries in the cache should never be NotPresent! If\n"
                  "this entry (%#x) is not tracked elsewhere, it will memory "
                  "leak here. Fix your protocol to eliminate these!",
                  address);
          }

            set[i] = entry;  // Init entry
            set[i]->m_Address = address;
            set[i]->m_Permission = AccessPermission_Invalid;
            DPRINTF(RubyCache, "Allocate clearing lock for addr: %x\n",
                    address);
            set[i]->m_locked = -1;
            m_tag_index[address] = i;
            entry->setSetIndex(cacheSet);
            entry->setWayIndex(i);

            if (touch) {
                m_replacementPolicy_ptr->touch(cacheSet, i, curTick());
            }

            return entry;
        }
    }

    panic("Allocate didn't find an available entry");
}


AbstractCacheEntry*
CacheMemory::allocate(Addr address, AbstractCacheEntry *entry, bool touch)
{
    assert(address == makeLineAddress(address));
    assert(!isTagPresent(address));
    assert(cacheAvail(address));
    DPRINTF(RubyCache, "address: %#x\n", address);

    if (address == 0x5d440){
      fprintf(stderr," We select 0x5d440 for allocate\n ");
    }


    // Find the first open slot
    int64_t cacheSet = addressToCacheSet(address);
    std::vector<AbstractCacheEntry*> &set = m_cache[cacheSet];
    for (int i = 0; i < m_cache_assoc; i++) {
        if (!set[i] || set[i]->m_Permission == AccessPermission_NotPresent) {
            if (set[i] && (set[i] != entry)) {
                warn_once("This protocol contains a cache entry handling bug: "
                    "Entries in the cache should never be NotPresent! If\n"
                    "this entry (%#x) is not tracked elsewhere, it will memory "
                    "leak here. Fix your protocol to eliminate these!",
                    address);
            }


            set[i] = entry;  // Init entry
            set[i]->m_Address = address;
            set[i]->m_Permission = AccessPermission_Invalid;
            DPRINTF(RubyCache, "Allocate clearing lock for addr: %x\n",
                    address);
            set[i]->m_locked = -1;
            m_tag_index[address] = i;
            m_tag_index_time[address] = curTick();
            entry->setSetIndex(cacheSet);
            entry->setWayIndex(i);

            if (touch) {
                m_replacementPolicy_ptr->touch(cacheSet, i, curTick());
            }

            return entry;
        }
    }

    panic("Allocate didn't find an available entry");
}

void CacheMemory::deallocateIcache(Addr address){
    assert(address == makeLineAddress(address));
    assert(isTagPresent(address));
    DPRINTF(RubyCache, "address: %#x\n", address);
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc != -1) {
       delete m_cache[cacheSet][loc];
       m_cache[cacheSet][loc] = NULL;
       m_tag_index.erase(address);
       m_tag_index_time.erase(address);
       assert(loc<100);
    }
}


void
CacheMemory::deallocate(Addr address)
{
    //modify this so that the cache also checks
    //the extra buffer
    assert(address == makeLineAddress(address));
    assert(isTagPresent(address));
    DPRINTF(RubyCache, "address: %#x\n", address);
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc != -1) {
        if (loc < 100) {
          //check that it is not in the swap list
          //or the miss buffer
          for (int i=0; i<buf_size; i++){
            if (miss_buffer[i].e)
              if (miss_buffer[i].e->m_Address == address )
                 panic("Line found in miss buf and set\n");
          }
          if (swap_list.size() != 0){
            auto it = swap_list.begin();
            Addr swap_addr= ((*it).set_addr);
            assert(swap_addr != address);
          }
          assert(m_cache[cacheSet][loc]);
          delete m_cache[cacheSet][loc];
          m_cache[cacheSet][loc] = NULL;
          m_tag_index.erase(address);
          m_tag_index_time.erase(address);
        } else {
          assert(miss_buffer[loc-100].e);
          assert(miss_buffer[loc-100].e->m_Address == address);
          delete miss_buffer[loc-100].e;
          if (swap_list.size() != 0){
             //check whether the address is available yet
             auto it = swap_list.begin();
             AbstractCacheEntry *e =
               m_cache[(*it).set][(*it).way];
             Addr set_addr = (*it).set_addr;
             if (e != NULL){ //storing potential side effect
               //check that the age is the lowest in the buffer
               checkAge(miss_buffer[loc-100].age);
               miss_buffer[loc-100].e = e;
               miss_buffer[loc-100].age = curTick();
               m_cache[(*it).set][(*it).way] = NULL;
               assert(m_tag_index.find(set_addr) !=
                      m_tag_index.end());
               m_tag_index.erase(m_tag_index.find(set_addr));
               m_tag_index_time.erase(set_addr);
             }else{ //No side effect
               miss_buffer[loc-100].e = NULL;
               m_side_effect_lost++;
             }
          }
          swap_list.clear();
        }
    }
}

void
CacheMemory::checkAge(uint64_t age){
  //we check that the age of the cache is what we expect
  uint64_t min_age = -1;
  for (int i=0; i<buf_size; i++){
     if (miss_buffer[i].e){
       if (min_age > miss_buffer[i].age){
          min_age = miss_buffer[i].age;
       }
     }
  }
  assert(min_age != -1);
  assert(min_age <= age);
}

Addr
CacheMemory::cacheProbeIcache(Addr address)
{
    assert(address == makeLineAddress(address));
    assert(!cacheAvailIcache(address));

    int64_t cacheSet = addressToCacheSet(address);
    return m_cache[cacheSet][m_replacementPolicy_ptr->getVictim(cacheSet)]->
        m_Address;
}


// Returns with the physical address of the conflicting cache line
Addr
CacheMemory::cacheProbe(Addr address)
{
    assert(address == makeLineAddress(address));
    //assert(!cacheAvail(address));

    //idea is to evict something and put it into
    //the buffer ... so something else ends up
    //getting evicted instead

    int64_t cacheSet = addressToCacheSet(address);
    int way = m_replacementPolicy_ptr->getVictim(cacheSet);
    Addr result;

    //if there is a buffer entry there, use it
    if (buffer_full){
      //get the oldest eviction candidate
      uint64_t temp = -1;
      int select = -1;
      for (int i=0; i<buf_size; i++){
        if (miss_buffer[i].e) {
           if (miss_buffer[i].age < temp) {
             temp = miss_buffer[i].age;
             select = i;
           }
        }
      }

      //look for an eviction candidate ...
      if (select != -1){
        result = miss_buffer[select].e->m_Address;
        assert(m_tag_index.find(address)
               == m_tag_index.end());
        if (swap_list.size() == 0){
           if (m_cache[cacheSet][way])
               swap_list.push_back(
               {m_cache[cacheSet][way]->m_Address,result,
                miss_buffer[select].e,cacheSet,way,select,address});
        }
        return result;
      }
    }

    //no buffer entry, then use the cache set
    assert(m_cache[cacheSet][way]);
    return m_cache[cacheSet][way]->m_Address;

}

// looks an address up in the cache
AbstractCacheEntry*
CacheMemory::lookup(Addr address)
{
    if (address != makeLineAddress(address)){
       //fprintf(stderr, "The address is %lx,"
       //                "The line address is %lx",
       //                address, makeLineAddress(address));
    }
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc == -1) return NULL;
    if (loc >= 100) {
      assert(miss_buffer[loc-100].e);
      return miss_buffer[loc-100].e;
    }
    assert(m_cache[cacheSet][loc]);
    return m_cache[cacheSet][loc];
}

// looks an address up in the cache
const AbstractCacheEntry*
CacheMemory::lookup(Addr address) const
{
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    if (loc == -1) return NULL;
    if (loc >= 100) {
      assert(miss_buffer[loc-100].e);
      return miss_buffer[loc-100].e;
    }
    assert(m_cache[cacheSet][loc]);
    return m_cache[cacheSet][loc];
}

// Sets the most recently used bit for a cache block
void
CacheMemory::setMRU(Addr address)
{
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc != -1){
        if (loc < 100) {
          m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
        } else {
           ; //don't update replacement policy
           assert(m_tag_index.find(address)
                  == m_tag_index.end());
        }
    }
}

void
CacheMemory::setMRU(const AbstractCacheEntry *e)
{
    uint32_t cacheSet = e->getSetIndex();
    uint32_t loc = e->getWayIndex();

    //check whether it is in the buffer, if so then
    for (int i = 0; i < buf_size ; i++){
       if (miss_buffer[i].e){
         if (miss_buffer[i].e == e){
            assert(m_tag_index.find(e->m_Address) ==
                   m_tag_index.end());
           return;
         }//no touch
       }
    }
    m_replacementPolicy_ptr->touch(cacheSet, loc, curTick());
}

void
CacheMemory::setMRU(Addr address, int occupancy)
{
    panic("We should not use this MRU strategy \n");
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);

    if (loc != -1) {
        if (m_replacementPolicy_ptr->useOccupancy()) {
            (static_cast<WeightedLRUPolicy*>(m_replacementPolicy_ptr))->
                touch(cacheSet, loc, curTick(), occupancy);
        } else {
            m_replacementPolicy_ptr->
                touch(cacheSet, loc, curTick());
        }
    }
}

int
CacheMemory::getReplacementWeight(int64_t set, int64_t loc)
{
    panic("We should not use getReplacementWeight\n");
    assert(set < m_cache_num_sets);
    assert(loc < m_cache_assoc);
    int ret = 0;
    if (m_cache[set][loc] != NULL) {
        ret = m_cache[set][loc]->getNumValidBlocks();
        assert(ret >= 0);
    }

    return ret;
}

void
CacheMemory::recordCacheContents(int cntrl, CacheRecorder* tr) const
{
    panic("We should not use recordCacheContents\n");
    uint64_t warmedUpBlocks = 0;
    uint64_t totalBlocks M5_VAR_USED = (uint64_t)m_cache_num_sets *
                                       (uint64_t)m_cache_assoc;

    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                AccessPermission perm = m_cache[i][j]->m_Permission;
                RubyRequestType request_type = RubyRequestType_NULL;
                if (perm == AccessPermission_Read_Only) {
                    if (m_is_instruction_only_cache) {
                        request_type = RubyRequestType_IFETCH;
                    } else {
                        request_type = RubyRequestType_LD;
                    }
                } else if (perm == AccessPermission_Read_Write) {
                    request_type = RubyRequestType_ST;
                }

                if (request_type != RubyRequestType_NULL) {
                    tr->addRecord(cntrl, m_cache[i][j]->m_Address,
                                  0, request_type,
                                  m_replacementPolicy_ptr->getLastAccess(i, j),
                                  m_cache[i][j]->getDataBlk());
                    warmedUpBlocks++;
                }
            }
        }
    }

    DPRINTF(RubyCacheTrace, "%s: %lli blocks of %lli total blocks"
            "recorded %.2f%% \n", name().c_str(), warmedUpBlocks,
            totalBlocks, (float(warmedUpBlocks) / float(totalBlocks)) * 100.0);
}

void
CacheMemory::print(ostream& out) const
{
    out << "Cache dump: " << name() << endl;
    for (int i = 0; i < m_cache_num_sets; i++) {
        for (int j = 0; j < m_cache_assoc; j++) {
            if (m_cache[i][j] != NULL) {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: " << *m_cache[i][j] << endl;
            } else {
                out << "  Index: " << i
                    << " way: " << j
                    << " entry: NULL" << endl;
            }
        }
    }
}

void
CacheMemory::printData(ostream& out) const
{
    out << "printData() not supported" << endl;
}

void
CacheMemory::setLocked(Addr address, int context)
{
    DPRINTF(RubyCache, "Setting Lock for addr: %#x to %d\n", address, context);
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    if (loc < 100){
      m_cache[cacheSet][loc]->setLocked(context);
    }else{
      assert(miss_buffer[loc-100].e);
      miss_buffer[loc-100].e->setLocked(context);
    }
}

void
CacheMemory::clearLocked(Addr address)
{
    //fprintf(stderr, "Doing clearLocked function\n");
    DPRINTF(RubyCache, "Clear Lock for addr: %#x\n", address);
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    if (loc < 100){
       m_cache[cacheSet][loc]->clearLocked();
    }else{
      assert(miss_buffer[loc-100].e);
      miss_buffer[loc-100].e->clearLocked();
    }
}

bool
CacheMemory::isLocked(Addr address, int context)
{
    //fprintf(stderr, "Doing isLocked function\n");
    assert(address == makeLineAddress(address));
    int64_t cacheSet = addressToCacheSet(address);
    int loc = findTagInSet(cacheSet, address);
    assert(loc != -1);
    DPRINTF(RubyCache, "Testing Lock for addr: %#llx cur %d con %d\n",
            address, m_cache[cacheSet][loc]->m_locked, context);
    if (loc < 100){
      return m_cache[cacheSet][loc]->isLocked(context);
    }else{
      assert(miss_buffer[loc-100].e);
      return miss_buffer[loc-100].e->isLocked(context);
    }
}

void
CacheMemory::regStats()
{
    SimObject::regStats();

    m_miss_buf_hits
        .name(name() + ".miss_buf_hits")
        .desc("Number of miss buffer hits")
        ;

    m_demand_hits
        .name(name() + ".demand_hits")
        .desc("Number of cache demand hits")
        ;

    m_side_effect_lost
        .name(name() + ".side_effect_lost")
        .desc("What if there was no replacement for evicted miss buf")
        ;


    m_demand_misses
        .name(name() + ".demand_misses")
        .desc("Number of cache demand misses")
        ;

    m_demand_accesses
        .name(name() + ".demand_accesses")
        .desc("Number of cache demand accesses")
        ;

    m_demand_accesses = m_demand_hits + m_demand_misses;

    m_sw_prefetches
        .name(name() + ".total_sw_prefetches")
        .desc("Number of software prefetches")
        .flags(Stats::nozero)
        ;

    m_hw_prefetches
        .name(name() + ".total_hw_prefetches")
        .desc("Number of hardware prefetches")
        .flags(Stats::nozero)
        ;

    m_prefetches
        .name(name() + ".total_prefetches")
        .desc("Number of prefetches")
        .flags(Stats::nozero)
        ;

    m_prefetches = m_sw_prefetches + m_hw_prefetches;

    m_accessModeType
        .init(RubyRequestType_NUM)
        .name(name() + ".access_mode")
        .flags(Stats::pdf | Stats::total)
        ;
    for (int i = 0; i < RubyAccessMode_NUM; i++) {
        m_accessModeType
            .subname(i, RubyAccessMode_to_string(RubyAccessMode(i)))
            .flags(Stats::nozero)
            ;
    }

    numDataArrayReads
        .name(name() + ".num_data_array_reads")
        .desc("number of data array reads")
        .flags(Stats::nozero)
        ;

    numDataArrayWrites
        .name(name() + ".num_data_array_writes")
        .desc("number of data array writes")
        .flags(Stats::nozero)
        ;

    numTagArrayReads
        .name(name() + ".num_tag_array_reads")
        .desc("number of tag array reads")
        .flags(Stats::nozero)
        ;

    numTagArrayWrites
        .name(name() + ".num_tag_array_writes")
        .desc("number of tag array writes")
        .flags(Stats::nozero)
        ;

    numTagArrayStalls
        .name(name() + ".num_tag_array_stalls")
        .desc("number of stalls caused by tag array")
        .flags(Stats::nozero)
        ;

    numDataArrayStalls
        .name(name() + ".num_data_array_stalls")
        .desc("number of stalls caused by data array")
        .flags(Stats::nozero)
        ;
}

// assumption: SLICC generated files will only call this function
// once **all** resources are granted
void
CacheMemory::recordRequestType(CacheRequestType requestType, Addr addr)
{
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            CacheRequestType_to_string(requestType));
    switch(requestType) {
    case CacheRequestType_DataArrayRead:
        if (m_resource_stalls)
            dataArray.reserve(addressToCacheSet(addr));
        numDataArrayReads++;
        return;
    case CacheRequestType_DataArrayWrite:
        if (m_resource_stalls)
            dataArray.reserve(addressToCacheSet(addr));
        numDataArrayWrites++;
        return;
    case CacheRequestType_TagArrayRead:
        if (m_resource_stalls)
            tagArray.reserve(addressToCacheSet(addr));
        numTagArrayReads++;
        return;
    case CacheRequestType_TagArrayWrite:
        if (m_resource_stalls)
            tagArray.reserve(addressToCacheSet(addr));
        numTagArrayWrites++;
        return;
    default:
        warn("CacheMemory access_type not found: %s",
             CacheRequestType_to_string(requestType));
    }
}

bool
CacheMemory::checkResourceAvailable(CacheResourceType res, Addr addr)
{
    panic("Should not use checkResourceAvailable\n");
    if (!m_resource_stalls) {
        return true;
    }

    if (res == CacheResourceType_TagArray) {
        if (tagArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Tag array stall on addr %#x in set %d\n",
                    addr, addressToCacheSet(addr));
            numTagArrayStalls++;
            return false;
        }
    } else if (res == CacheResourceType_DataArray) {
        if (dataArray.tryAccess(addressToCacheSet(addr))) return true;
        else {
            DPRINTF(RubyResourceStalls,
                    "Data array stall on addr %#x in set %d\n",
                    addr, addressToCacheSet(addr));
            numDataArrayStalls++;
            return false;
        }
    } else {
        panic("Unrecognized cache resource type.");
    }
}

bool
CacheMemory::isBlockInvalid(int64_t cache_set, int64_t loc)
{
  panic("Should not use isBlockInvalid\n");
  return (m_cache[cache_set][loc]->m_Permission == AccessPermission_Invalid);
}

bool
CacheMemory::isBlockNotBusy(int64_t cache_set, int64_t loc)
{
  panic("Should not use isBlockNotBusy\n");
  return (m_cache[cache_set][loc]->m_Permission != AccessPermission_Busy);
}
