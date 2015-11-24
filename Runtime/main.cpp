#include "Runtime.h"
#include <stdio.h>
#include <map>
#include <string.h>
#include <memory>
#include <stack>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../GC/GC.h"
#define DEBUGMODE

void* gc;




class BStream {
public:
  unsigned char* ptr;
  size_t len;
  BStream() {
  }
  BStream(void* ptr, size_t sz) {
    this->ptr = (unsigned char*)ptr;
    this->len = sz;
  }
  void Read(void* output, size_t len) {
    if(len>this->len) {
      throw "counterclockwise"; //How is this even possible?
    }
    
    memcpy(output,ptr,len);
    this->len-=len;
    this->ptr+=len;
  }
  template<typename T>
  T& Read(T& val) {
    if(sizeof(val)>len) {
      throw "down";
    }
    memcpy(&val,ptr,sizeof(val));
    this->len-=sizeof(val);
    this->ptr+=sizeof(val);
    return val;
  }
  void* Increment(size_t len) {
    if(len>this->len) {
      throw "up"; //barf
    }
    this->len-=len;
    this->ptr+=len; 
  }
  char* ReadString() {
    char* start = (char*)this->ptr;
    size_t len = strnlen((char*)this->ptr,this->len)+1;
    if(len>this->len) {
      throw "away your assembly code. Trust me. It's NO good!";
    }
    this->ptr+=len;
    this->len-=len;
    return start;
  }
  
};


class UALModule; //forward-declaration


/**
 * @summary A safe garbage collected handle object for C++
 * */
template<typename T>
class SafeGCHandle {
public:
  T** obj;
  SafeGCHandle(T** objref) {
    this->obj = objref;
    GC_Mark(gc,objref,true);
  }
  ~SafeGCHandle() {
    GC_Unmark(gc,obj,true);
  }
};



/**
 * @summary A raw array of contiguous garbage-collected objects
 * */
class RawObjectArray {
public:
  void** array;
  size_t count;
  /**
   * @summary Constructs an array of garbage-collected objects of the specified size
   * @param count The number of elements in the array
   * */
  ObjectArray(size_t count) {
    GC_Allocate(gc,sizeof(size_t)*count,0,&array,0);
    GC_Mark(gc,&array,true);
  }
  
  ~ObjectArray() {
    GC_Unmark(gc,&array,true);
  }
};


class UALMethod {
public:
  BStream str;
  UALMethod(BStream& str) {
    this->str = str;
  }
  /**
   * @summary Invokes this method with the specified arguments
   * @param args Garbage collected array of arguments
   * @param count The number of arguments being passed to the function
   * */
  void Invoke(void** args, size_t count) {
    SafeGCHandle<void**> handle(&args);
    BStream reader = str;
    unsigned char opcode;
    while(opcode != 255) {
      case 0:
	//Load argument (argnum)
	
	break;
    }
  }
}; 

class UALType {
public:
  
  BStream bstr; //in-memory view of file
  bool compiled; //Whether or not this module has been compiled or interpreted
  UALModule* module;
  std::map<std::string,UALMethod*> methods;
  UALType(BStream& str, UALModule* module) {
    bstr = str;
    compiled = false;
    this->module = module;
    
  }
  /**
   * @summary Compiles this UAL type to native code (x86)
   * */
  void Compile() {
    uint32_t count;
    bstr.Read(count);
    size_t nativeCount = count; //Copy to size_t for faster performance
    for(size_t i = 0;i<nativeCount;i++) {
      methods[bstr.ReadString()] = new UALMethod(bstr);
    }
  }
};

class UALModule {
public:
  std::map<std::string,UALType*> types;
  UALModule(void* bytecode, size_t len) {
    BStream str(bytecode,len);
    uint32_t count;
    str.Read(count);
#ifdef DEBUGMODE
    printf("Reading in %i classes\n",(int)count);
#endif
    for(uint32_t i = 0;i<count;i++) {
      char* name = str.ReadString();

      uint32_t asmlen;
      str.Read(asmlen);
      
      BStream obj(str.Increment(asmlen),asmlen);
      #ifdef DEBUGMODE
      printf("Loading %s\n",name);
      #endif
      types[std::string(name)] = new UALType(obj,this);
    }
    
  }
};


int main(int argc, char** argv) {
  int fd = open(argv[1],O_RDONLY);
  struct stat us; //It's a MAC (status symbol)
  fstat(fd,&us);
  size_t len = us.st_size;
  void* ptr = mmap(0,len,PROT_READ,MAP_SHARED,fd,0);
  gc = GC_Init(3);
  UALModule* module = new UALModule(ptr,len);
  return 0;
}
