#include "Runtime.h"
#include <stdio.h>
#include <map>
#include <string.h>
class BStream {
public:
  void* ptr;
  size_t len;
  BStream(void* ptr, size_t sz) {
    this->ptr = ptr;
    this->len = sz;
  }
  void Read(void* output, size_t len) {
    if(len>this->len) {
      throw "counterclockwise";
    }
    
    memcpy(output,ptr,len);
    this->len-=len;
    ((unsigned char*)this->ptr)+=len;
  }
  template<typename T>
  void Read(T& val) {
    if(sizeof(val)>len) {
      throw "down";
    }
    memcpy(&val,ptr,sizeof(val));
    this->len-=sizeof(val);
    ((unsigned char*)this->ptr)+=sizeof(val);
  }
  void* Increment(size_t len) {
    if(len>this->len) {
      throw "up"; //barf
    }
    this->len-=len;
    ((unsigned char*)this->ptr)+=len; 
  }
  char* ReadString() {
    size_t len = strnlen((char*)this->ptr,this->len);
    if(len>this->len) {
      throw "away your assembly code. Trust me. It's NO good!";
    }
    ((unsigned char*)this->ptr)+=len;
    this->len-=len;
    return this->ptr;
  }
  
};

class UALType {
public:
  
  UALType(BStream& str) {
    
  }
};

class UALModule {
public:
  std::map<std::string,std::shared_ptr<UALType>> types;
  UALModule(void* bytecode, size_t len) {
    BStream str(bytecode,len);
    uint32_t count;
    str.Read(count);
    for(uint32_t i = 0;i<count;i++) {
      char* name = str.ReadString();
      uint32_t asmlen;
      str.Read(asmlen);
      BStream obj(str.Increment(asmlen),asmlen);
      types[name] = std::make_shared<UALType>(obj);
      
    }
  }
};


int main(int argc, char** argv) {
  
  return 0;
}
