

#ifndef __CPU_O3_COMMIT_LOAD_HH__
#define __CPU_O3_COMMIT_LOAD_HH__


typedef struct commitLoadInfo {
  InstSeqNum seqNum; //each committed load has a unique seqnum
  Addr lowAddr;      //each load has one or two addresses (may not be unique)
  Addr highAddr;
  bool isMiss;       //was it a cache miss

  public:
     bool operator<(const commitLoadInfo& t) const
     {
        //we commit the information that we require
        return (this->seqNum < t.seqNum);
     }

     bool operator==(const InstSeqNum& t) const
     {
        //we commit the information that we require
        return (this->seqNum == t);
     }


} commitLoadInfo;

#endif
