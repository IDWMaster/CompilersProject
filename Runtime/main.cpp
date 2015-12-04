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
    void* retval = this->ptr;
    if(len>this->len) {
      throw "up"; //barf
    }
    this->len-=len;
    this->ptr+=len; 
    return retval;
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
class SafeGCHandle {
public:
  void** obj;
  template<typename T>
  SafeGCHandle(T objref) {
    this->obj = (void**)objref;
    GC_Mark(gc,(void**)objref,true);
  }
  ~SafeGCHandle() {
    GC_Unmark(gc,obj,true);
  }
};




typedef struct {
  size_t count; //The number of elements in the array
  size_t stride; //The size of each element in the array (in bytes), or zero if array of objects
} GC_Array_Header;

typedef struct {
  uint32_t length;
} GC_String_Header;


/**
 * Creates an array of primitives
 * */
template<typename T>
static inline void GC_Array_Create_Primitive(GC_Array_Header*& output, size_t count) {
    GC_Allocate(gc,sizeof(GC_Array_Header)+(sizeof(T)*count),0,(void**)&output,0);
    output->count = count;
    output->stride = sizeof(T);
}
/**
 * Creates an array of a managed datatype
 * */
static inline void GC_Array_Create(GC_Array_Header*& output, size_t count) {
  GC_Allocate(gc,sizeof(GC_Array_Header),count,(void**)&output,0);
  output->count = count;
  output->stride = 0;
}
/**
 * Creates a String from a C-string
 * */
static inline void GC_String_Create(GC_String_Header*& output, const char* cstr) {
  //NOTE: This is out-of-spec. According to the ECMA specification for .NET -- strings should be encoded in UTF-16 format. Also; NULL-terminating the string isn't typical either; but whatever.
  GC_Allocate(gc,sizeof(GC_String_Header)+strlen(cstr)+1,0,(void**)&output,0);
  output->length = strlen(cstr);
  memcpy(output+1,cstr,strlen(cstr)+1);
}
/**
 * Converts a String to a C-string
 * */
static inline const char* GC_String_Cstr(GC_String_Header* ptr) {
  return (const char*)(ptr+1);
}



/**
 * Retrieve non-managed object from array (unsafe)
 * */
template<typename T>
static inline T& GC_Array_Fetch(GC_Array_Header* header,size_t index) {
  return ((T*)(header+1))[index];
}
/**
 * Retrieve managed object from array (unsafe)
 * */
static inline void* GC_Array_Fetch(GC_Array_Header* header, size_t index) {
  void** array = (void**)(header+1);
  return array[index];
}

/**
 * Sets an object value in an array of managed objects
 * */
template<typename T>
static inline void GC_Array_Set(GC_Array_Header* header, size_t index, T* value) {
  void** array = (void**)(header+1);
  if(array[index]) {
    GC_Unmark(gc,array+index,false);
  }
  array[index] = value;
  GC_Mark(gc,array+index,false);
}


class StackEntry {
public:
  unsigned char entryType; //Entry type
  /**
   * 0 -- Undefined
   * 1 -- Managed object (pointer)
   * 
   * */
  
  
  void* value; //Value
  StackEntry() {
    entryType = 0;
    value = 0;
  }
  void PutObject(void* obj) {
    value = obj;
    entryType = 1;
    GC_Mark(gc,&obj,true);
  }
  void Release() {
    if(entryType == 1) {
      GC_Unmark(gc,&value,true);
    }
  }
  
};
class UALMethod {
public:
  BStream str;
  bool isManaged;
  UALMethod(const BStream& str) {
    this->str = str;
    this->str.Read(isManaged);
  }
  /**
   * @summary Invokes this method with the specified arguments
   * @param args Stack-allocated array of arguments
   * */
  void Invoke(void** args) {
    StackEntry frame[10]; //No program should EVER need more than 10 frames..... Of course; they said that about RAM way back in the day.....
    StackEntry* position = frame;
    
    BStream reader = str;
    unsigned char opcode;
    while(reader.Read(opcode) != 255) {
      printf("EXEC OP: %i\n",(int)opcode);
      switch(opcode) {
	case 0:
	  //TODO: Make this work with things other than Objects.
	{
	  uint32_t index;
	  reader.Read(index);
	  position->PutObject(args[index]);
	  position++;
	}
	  break;
	case 1:
	  //TODO: Call function
	  uint32_t funcID;
	  reader.Read(funcID);
	  printf("TODO: Function call not yet implemented (ID %i)\n",funcID);
	  break;
	case 2:
	{
	  //Push string onto stack
	  GC_String_Header* obj;
	  GC_String_Create(obj,reader.ReadString());
	  position->PutObject(obj);
	  position++;
	}
	  break;
	case 10:
	  break;
	default:
	  printf("ERR: Illegal OPCODE %i\n",(int)opcode);
	  abort();
	  break;
      }
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
   * @summary Compiles this UAL type to native code (x86), or interprets
   * */
  void Compile() {
    if(!compiled) {
    uint32_t count;
    bstr.Read(count);
    printf("Reading %i methods\n",count);
    size_t nativeCount = count; //Copy to size_t for faster performance
    for(size_t i = 0;i<nativeCount;i++) {
      const char* mname = bstr.ReadString();
      uint32_t mlen;
      bstr.Read(mlen);
      printf("%s of length %i\n",mname,(int)mlen);
      
      void* ptr = bstr.Increment(mlen);
      methods[mname] = new UALMethod(BStream(ptr,mlen));
    }
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
  void LoadMain(int argc, char** argv) {
    //Find main
      UALType* mainClass = 0;
      for(auto i = types.begin();i!= types.end();i++) {
	i->second->Compile();
	if(i->second->methods.find("Main") != i->second->methods.end()) {
	  //Found it!
	  mainClass = i->second;
	}
      }
      if(mainClass == 0) {
	printf("Error: Unable to find Main.\n");
	return;
      }
      //Invoke main
      GC_Array_Header* array;
      GC_Array_Create(array,argc);
      SafeGCHandle arrhandle(&array);
      for(size_t i = 0;i<array->count;i++) {
	GC_String_Header* managedString;
	GC_String_Create(managedString,argv[i]);
	SafeGCHandle stringHandle(&managedString);
	GC_Array_Set(array,i,managedString);
      }
      mainClass->methods["Main"]->Invoke((void**)&array);
  }
};


int main(int argc, char** argv) {
  
  int fd = 0;
  if(argc>1) {
  fd = open(argv[1],O_RDONLY);
  }else {
    //Debug mode, open ual.out in current directory
    fd = open(argv[1],O_RDONLY);
  }
  struct stat us; //It's a MAC (status symbol)
  fstat(fd,&us);
  size_t len = us.st_size;
  void* ptr = mmap(0,len,PROT_READ,MAP_SHARED,fd,0);
  gc = GC_Init(3);
  UALModule* module = new UALModule(ptr,len);
  module->LoadMain(argc-1,argv+1);
  return 0;
}
