#include "Runtime.h"
#include <stdio.h>
#include <map>
#include <string.h>
#include <memory>
#include <stack>
#include <vector>
#include <complex>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
//#define GC_FAKE
#include "../GC/GC.h"
#include <set>
#include "../asmjit/src/asmjit/asmjit.h"
#define DEBUGMODE

void* gc;

//BEGIN PLATFORM CODE

asmjit::JitRuntime* JITruntime;
asmjit::X86Compiler* JITCompiler;


//END PLATFORM CODE




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
    GC_Mark((void**)objref,true);
  }
  ~SafeGCHandle() {
    GC_Unmark(obj,true);
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
 * @summary Reads white space
 * @param found True if whitespace was found, false otherwise
 * @returns The string up to the whitespace
 * */
static std::string Parser_ExpectWhitespace(const char*& str, bool& found) {
  std::string retval; //std::string is slow. We could optimize this; but assembly loading/JITting or interpreting is expected to be slow at initial load with most runtimes. Performance shouldn't matter too much at early loading stage.
  found = false;
  while(*str != 0) {
    if(isspace(*str)) {
      while(isspace(*str)){str++;}
      found = true;
      return retval;
    }else {
      retval+=*str;
    }
    str++;
  }
  return retval;
}

/**
 * @summary Expects a specific character
 * @param acter The character to expect
 * @param found True if the specified character was found, false otherwise.
 * @returns The text up to the specified character
 * */
static std::string Parser_ExpectChar(const char*& str, char acter, bool& found) {
  std::string retval;
  while(*str != 0) {
    if(*str == acter) {
      str++;
      return retval;
    }
    retval+=*str;
    str++;
  }
  return retval;
}


/**
 * @summary Expects a list of characters
 * @param acters A list of characters to search for, followed by a NULL terminator
 * @param found The memory address of the character which was found, or NULL if the character was not found.
 * @returns The text up to the specified character
 * */
static std::string Parser_ExpectMultiChar(const char*& str, const char* acters, const char*& found) {
  std::string retval;
  found = 0;
  while(*str != 0) {
    const char* current = acters;
    while(*current != 0) {
    if(*str == *current) {
      found = str;
      str++;
      return retval;
    }
    current++;
    }
    retval+=*str;
    str++;
  }
  return retval;
}

/**
 * @summary Expects a string
 * @param acter The string to expect
 * @param _found True if the specified string was found, false otherwise.
 * @returns The text up to the specified string
 * */
static std::string Parser_ExpectString(const char*& str, const char* acter, bool& found) {
  std::string retval;
  while(*str != 0) {
    if(*str == acter[0]) {
      const char* cpos = acter;
      found = true;
      while(*str != 0) {
	if(*cpos == 0) {
	  //Found whole string
	  found = true;
	  return retval;
	}
	if(*str != *cpos) {
	  found = false;
	  break;
	}
	str++;
	cpos++;
      }
      str++;
      return retval;
    }
    retval+=*str;
    str++;
  }
  return retval;
}


/**
 * Contains information about a method signature
 * */
class MethodSignature {
public:
  std::string fullSignature;
  std::string returnType; //Return type of method
  std::string className; //Fully-qualified class name of method
  std::string methodName;
  std::vector<std::string> args;
  MethodSignature() {
  }
  MethodSignature(const char* rawName) {
    fullSignature = rawName;
    bool found;
   returnType = Parser_ExpectWhitespace(rawName,found);
   className = Parser_ExpectString(rawName,"::",found);
   methodName = Parser_ExpectChar(rawName,'(',found);
   
   while(true) {
     const char* foundChar;
     std::string argType = Parser_ExpectMultiChar(rawName,",)",foundChar);
     if(argType.size()) {
      args.push_back(argType);
     }
     if(*foundChar == ')') {
       //End of arguments
       break;
     }
   }
  }
  bool operator<(const MethodSignature& other) const {
    return other.fullSignature<fullSignature;
  }
};





/**
 * Creates an array of primitives
 * */
template<typename T>
static inline void GC_Array_Create_Primitive(GC_Array_Header*& output, size_t count) {
    GC_Allocate(sizeof(GC_Array_Header)+(sizeof(T)*count),0,(void**)&output,0);
    output->count = count;
    output->stride = sizeof(T);
}
/**
 * Creates an array of a managed datatype
 * */
static inline void GC_Array_Create(GC_Array_Header*& output, size_t count) {
  GC_Allocate(sizeof(GC_Array_Header),count,(void**)&output,0);
  output->count = count;
  output->stride = 0;
}




/**
 * Creates a String from a C-string
 * */
static inline void GC_String_Create(GC_String_Header*& output, const char* cstr) {
  //NOTE: This is out-of-spec. According to the ECMA specification for .NET -- strings should be encoded in UTF-16 format. Also; NULL-terminating the string isn't typical either; but whatever.
  GC_Allocate(sizeof(GC_String_Header)+strlen(cstr)+1,0,(void**)&output,0);
  output->length = strlen(cstr);
  memcpy(output+1,cstr,output->length+1);
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
    GC_Unmark(array+index,false);
  }
  array[index] = value;
  //TODO: BUG This is causing string truncation
  GC_Mark(array+index,false);
}



class Type {
public:
  size_t size; //The total size of this type (used when allocating memory)
  std::map<std::string,Type*> fields; //Fields in this type
  std::string name; //The fully-qualified name of the type
  bool isStruct; //Whether or not this type should be treated as a struct or a managed object.
  virtual ~Type(){};
};

class StackEntry {
public:
  unsigned char entryType; //Entry type
  /**
   * 0 -- Undefined
   * 1 -- Managed object (pointer)
   * */
  
  
  void* value; //Value
  StackEntry() {
    entryType = 0;
    value = 0;
    type = 0;
  }
  void PutObject(void* obj) {
    value = obj;
    entryType = 1;
    GC_Mark(&value,true);
  }
  void Release() {
    if(entryType == 1) {
      GC_Unmark(&value,true);
    }
  }
  Type* type;
};

class UALMethod;
static UALMethod* ResolveMethod(void* assembly, uint32_t handle);
static std::map<std::string,void*> abi_ext;

static void ConsoleOut(GC_String_Header* str) {
  const char* mander = GC_String_Cstr(str); //Charmander is a constant. Always.
  printf("%s",mander);
}
static void PrintDouble(uint64_t encoded) { //Print a double on the double.
  double onthe = *(double*)&encoded;
  printf("%f\n",onthe);
}

static void PrintInt(int eger) {
  printf("%i",eger);
}

static void Ext_Invoke(const char* name, GC_Array_Header* args) {
  ((void(*)(GC_Array_Header*))abi_ext[name])(args);
}


Type* ResolveType(const char* name);


//A parse tree node

enum NodeType {
  CallNode
};
class Node {
public:
  NodeType type;
  Node() {
  }
  virtual ~Node(){};
};

//Objects are referred to when possible by reference identifier strings
//to allow for dynamic modules to be loaded and unloaded without requiring recompilation of existing code,
//and to allow for recompilation of a method if necessary for whatever reason.
class CallNode:public Node {
public:
  std::string methodName;
  CallNode() {
    type = NodeType::CallNode;
  }
  
};






class DeferredOperation {
public:
  virtual void Run(const asmjit::X86GpVar& output) = 0;
  virtual ~DeferredOperation(){};
};
template<typename T>
class DeferredOperationFunctor:public DeferredOperation {
public:
  T functor;
DeferredOperationFunctor(const T& func):functor(func) {
}
  void Run(const asmjit::X86GpVar& output) {
    functor(output);
  }
};

template<typename T>
static DeferredOperation* MakeDeferred(const T& functor) {
  return new DeferredOperationFunctor<T>(functor);
}

class UALMethod {
public:
  BStream str;
  bool isManaged;
  void* assembly;
  MethodSignature sig;
  uint32_t localVarCount;
  std::vector<std::string> locals;
  asmjit::X86Compiler* JITCompiler;
  
  UALMethod(const BStream& str, void* assembly, const char* sig) {
    this->JITCompiler = new asmjit::X86Compiler(JITruntime);
    this->sig = sig;
    this->str = str;
    this->str.Read(isManaged);
    if(isManaged) {
      this->str.Read(localVarCount);
      locals.resize(localVarCount);
      for(size_t i = 0;i<localVarCount;i++) {
	locals[i] = this->str.ReadString();
      }
    }
    this->assembly = assembly;
    nativefunc = 0;
  }
  void Compile() {
    
    //Compile UAL to x64
    unsigned char opcode;
    BStream reader = str;
    auto GetOffset = [&](){
      return ((size_t)reader.ptr)-((size_t)str.ptr);
    };
    std::map<size_t,asmjit::Label> UALTox64Offsets;
    std::map<size_t,asmjit::Label> pendingRelocations;
    
    
    
    auto addInstruction = [&](){
      size_t ualpos = GetOffset()-1;
      asmjit::Label instruction;
      if(pendingRelocations.find(ualpos) != pendingRelocations.end()) {
	instruction = pendingRelocations[ualpos];
	JITCompiler->bind(instruction);
	pendingRelocations.erase(ualpos);
      }else {
	instruction = JITCompiler->newLabel();
	JITCompiler->bind(instruction);
      }
      UALTox64Offsets[ualpos] = instruction;
      
    };
    asmjit::FuncBuilderX builder;
    builder.setRet(asmjit::kVarTypeIntPtr);
    builder.addArg(asmjit::kVarTypeIntPtr);
    JITCompiler->addFunc(asmjit::kFuncConvHost,builder);
    
    
    
    //Stack size = localVarCount+1 temporary store
    size_t stackmem_tempoffset = (localVarCount)*sizeof(size_t);
    asmjit::X86Mem stackmem = JITCompiler->newStack((localVarCount+2)*sizeof(size_t),sizeof(size_t));
    
    
    
    
    //asmjit::X86GpVar arglist = JITCompiler->newGpVar(); //NOTE: Old calling convention (used GC_Array_Header of arguments)
    //JITCompiler->setArg(0,arglist);
    StackEntry frame[10];
    StackEntry* position = frame;
    
    
    //Calling convention:
    //Each argument is word size of processor
    //For Object types, pass memory address in register (if available)
    //TODO: Struct handling
    
    /**
     * @summary Pops a variable from the stack, and puts it in the specified register or memory location
     * */
    auto pop = [&](asmjit::X86GpVar& location) {
      position--;
       switch(position->entryType) {
	 case 0:
	 {
	   //Load value from argument
	   uint32_t argidx = (uint32_t)(size_t)position->value;
	   JITCompiler->setArg(argidx,location);
	 }
	   break;
	 case 1:
	 {
	   //TODO: IMPORTANT NOTE:
	   //We've found the cause of the problem -- NewStack can only be called once per function.
	   //We need to know the stack size ahead of time.
	   
	   
	   //Load string immediate
	   //Need to call GC_String_Create with address of C string
	   asmjit::X86GpVar stackptr = JITCompiler->newGpVar();
	   asmjit::X86GpVar stringptr = JITCompiler->newGpVar();
	   JITCompiler->lea(stackptr,stackmem); //Load effective address of stack memory (contains pointer to GC_String_Header)
	   JITCompiler->add(stackptr,asmjit::imm(stackmem_tempoffset));
	   
	   JITCompiler->mov(stringptr,asmjit::imm((size_t)position->value));
	   printf("Load string:%s:\n",position->value);
	   asmjit::FuncBuilderX builder;
	   builder.addArg(asmjit::kVarTypeIntPtr);
	   builder.addArg(asmjit::kVarTypeIntPtr);
	   
	   //Create managed string from C-string
	   asmjit::X86CallNode* funccall = JITCompiler->call((size_t)&GC_String_Create,asmjit::kFuncConvHost,builder);
	   funccall->setArg(0,stackptr);
	   funccall->setArg(1,stringptr);
	   //MOVE managed object
	   JITCompiler->mov(location,JITCompiler->intptr_ptr(stackptr)); //MOVImm ManagedObject
	 }
	   break;
	 case 2:
	 {
	   //Load 32-bit word immediate
	   JITCompiler->mov(location,asmjit::imm((int)(ssize_t)position->value));
	 }
	   break;
	 case 3:
	 {
	   //Pop from local variable
	   size_t varidx = (size_t)position->value;
	   asmjit::X86GpVar temp = JITCompiler->newGpVar();
	   JITCompiler->lea(temp,stackmem);
	   JITCompiler->add(temp,varidx*sizeof(size_t));
	   JITCompiler->mov(location,JITCompiler->intptr_ptr(temp));
	 }
	   break;
	 case 4:
	 {
	   DeferredOperation* dop = (DeferredOperation*)position->value;
	   dop->Run(location);
	   delete dop;
	 }
	   break;
	 case 5:
	 {
	   //We're loading the right values here....
	   //Load FP immediate
	   JITCompiler->mov(location,asmjit::imm((uint64_t)position->value));
	 }
	   break;
       }
       
    };
    //Execute a write barrier mark at the location specified in this register.
    auto mark = [&](asmjit::X86GpVar& location, bool isRoot) {
      asmjit::FuncBuilderX builder;
      builder.addArg(asmjit::kVarTypeIntPtr);
      builder.addArg(asmjit::kVarTypeInt8);
      asmjit::X86CallNode* funccall = JITCompiler->call((size_t)&GC_Mark,asmjit::kFuncConvHost,builder);
      funccall->setArg(0,location);
      funccall->setArg(1,asmjit::imm(isRoot));
      
    };
    auto unmark = [&](asmjit::X86GpVar& location, bool isRoot) {
      asmjit::FuncBuilderX builder;
      builder.addArg(asmjit::kVarTypeIntPtr);
      builder.addArg(asmjit::kVarTypeInt8);
      asmjit::X86CallNode* funccall = JITCompiler->call((size_t)&GC_Unmark,asmjit::kFuncConvHost,builder);
      
      funccall->setArg(0,location);
      funccall->setArg(1,asmjit::imm(isRoot));
      
      
    };
    auto MakeLabel = [&]() {
      asmjit::Label retval = JITCompiler->newLabel();
      JITCompiler->bind(retval);
      return retval;
    };
    
    auto GetPosition = [&]() {
      return position;
    };
    
    while(reader.Read(opcode) != 255) {
      printf("OPCODE: %i\n",(int)opcode);
      switch(opcode) {
	case 0:
	{
	  //Push argument to evaluation stack
	  uint32_t index;
	  reader.Read(index);
	  position->entryType = 0;
	  position->value = (void*)index;
	  std::string tname = sig.args[index];
	  position->type = ResolveType(tname.data());
	  position++;
	}
	  break;
	case 1:
	  //Call function
	{
	  addInstruction();
	  
	  uint32_t funcID;
	  reader.Read(funcID);
	  UALMethod* method = ResolveMethod(assembly,funcID);
	  size_t argcount = method->sig.args.size();
	  asmjit::FuncBuilderX methodsig;
	  asmjit::X86CallNode* call;
	  
	  
	  //Stack memory for managed objects. For now; we'll assume that they're all managed.
	  asmjit::X86GpVar* realargs = new asmjit::X86GpVar[argcount];
	  
	    asmjit::X86GpVar temp = JITCompiler->newGpVar();
	  for(size_t i = 0;i<argcount;i++) {
	    realargs[i] = JITCompiler->newGpVar();  
	    pop(realargs[i]);
	  }
	  
	  
	  for(size_t i = 0;i<argcount;i++) {
	    
	      methodsig.addArg(asmjit::kVarTypeIntPtr);
	    
	  }
	  if(method->nativefunc) {
	    
	    printf("EXPERIMENTAL: Invoke managed function\n");
	    call = JITCompiler->call((size_t)method->nativefunc,asmjit::kFuncConvHost,methodsig);
	  }else {
	    //call = JITCompiler->call()
	    void* funcptr = (void*)abi_ext[method->sig.methodName.data()];
	    call = JITCompiler->call((size_t)funcptr,asmjit::kFuncConvHost,methodsig);
	  }
	  for(size_t i = 0;i<argcount;i++) {
	      call->setArg(i,realargs[i]);
	  }
	  
	  delete[] realargs;
	  
	}
	  break;
	case 2:
	  //Load string
	{
	  addInstruction();
	  
	  position->value = reader.ReadString();
	  position->entryType = 1; //Load string
	  position->type = ResolveType("System.String");
	  position++;
	}
	  break;
	case 3:
	{
	  //RET
	  JITCompiler->ret();
	  
	}
	  break;
	case 4:
	{
	  //Load 32-bit integer immediate
	  addInstruction();
	  uint32_t val;
	  reader.Read(val);
	  position->entryType = 2; //Load 32-bit word
	  position->type = ResolveType("System.Int32");
	  position->value = (void*)(uint64_t)val;
	  position++;
	}
	  break;
	case 5:
	  //stloc
	{
	  uint32_t index;
	  addInstruction();
	  reader.Read(index);
	  
	  asmjit::X86GpVar temp = JITCompiler->newGpVar();
	  JITCompiler->lea(temp,stackmem);
	  JITCompiler->add(temp,asmjit::imm((size_t)(sizeof(size_t)*index)));
	  //TODO: Write barrier if managed object
	  asmjit::X86GpVar valreg = JITCompiler->newGpVar();
	  pop(valreg);
	  JITCompiler->mov(JITCompiler->intptr_ptr(temp),valreg);
	  std::string tname = this->locals[index];
	  if(!ResolveType(tname.data())->isStruct) {
	    mark(temp,true);
	  }
	  
	  
	}
	  break;
	case 6:
	  //Branch to str.ptr+offset
	{
	  addInstruction();
	  uint32_t offset;
	  reader.Read(offset);
	  //TODO: Crashes on ondiscovered offset. Patch this later (defer) somehow.
	  
	   if(UALTox64Offsets.find(offset) == UALTox64Offsets.end()) {
	     asmjit::Label jmpOffset = JITCompiler->newLabel();
	    pendingRelocations[offset] = jmpOffset;
	    JITCompiler->jmp(jmpOffset);
	  }else {
	    JITCompiler->jmp(UALTox64Offsets[offset]); //Make Link jump! (NO!!!!! It's Zelda!)
	   }
	}
	  break;
	case 7:
	{
	  //LDLOC
	  addInstruction();
	  uint32_t id;
	  reader.Read(id);
	  //TODO: Push local variable onto stack
	  position->entryType = 3;
	  position->value = (void*)(size_t)id;
	  std::string tname = this->locals[id];
	  position->type = ResolveType(tname.data());
	  position++;
	}
	  break;
	case 8:
	{
	  //Add values TODO type check
	  //NOTE: We can only perform addition on native data types; not managed ones.
	  
	  
	  //TODO: Maybe we can defer execution of this whole segment until it is needed somehow?
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b);
	    pop(output);
	    if(GetPosition()->type == ResolveType("System.Double")) {
	      
	      //NOTE: The FPU is sort of like a separate processor.
	      //The FPU has its own set of registers, and can only transfer data through the main memory bus of the chip.
	      //Therefore it is not possible to transfer values directly from the FPU to CPU registers; or vice-versa.
	      //So; unfortunately, we will have to transfer from our source registers, to memory, then to the FPU.
	      //The current optimizing engine won't be able to optimize this out, so floating point operations will be incredibly slow.
	      //This will be fixed when (and if) I add a new optimizer.
	      
	      //Temp 0, 1 = address of temporaries on stack
	      asmjit::X86GpVar temp = JITCompiler->newGpVar();
	      JITCompiler->lea(temp,stackmem);
	      JITCompiler->add(temp,stackmem_tempoffset); //Compute the address of the stack start
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp),output);
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp,8),b);
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp));
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp,8));
	      JITCompiler->faddp(); //Eat your Raspberry Pi.
	      JITCompiler->fst(JITCompiler->intptr_ptr(temp));
	      JITCompiler->mov(output,JITCompiler->intptr_ptr(temp));
	      
	    }else {
	      JITCompiler->add(output,b);
	    }
	    
	  });
	  
	  //TODO: Push result to stack
	  //TODO: This is a bad idea. It messes up RSP and causes stuff to be written to the wrong place.
	  position->entryType = 4;
	  position->value = op;
	  position++;
	}
	  break;
	case 9:
	{
	  //BLE!!!!!! (emit barfing sound here into the assembly code)
	  addInstruction();
	  asmjit::X86GpVar v0 = JITCompiler->newGpVar();
	  asmjit::X86GpVar v1 = JITCompiler->newGpVar();
	  pop(v0);
	  pop(v1);
	  JITCompiler->cmp(v1,v0);
	  uint32_t offset;
	  reader.Read(offset);
	  if(UALTox64Offsets.find(offset) != UALTox64Offsets.end()) {
	    JITCompiler->jle(UALTox64Offsets[offset]);
	  }else {
	    asmjit::Label label = JITCompiler->newLabel();
	    pendingRelocations[offset] = label;
	    JITCompiler->jle(label);
	    
	  }
	  
	}
	  break;
	case 10:
	  //NOPE. Not gonna happen.
	{
	  addInstruction();
	}
	  break;
	case 11:
	  {
	  //BEQ
	  addInstruction();
	  asmjit::X86GpVar v0 = JITCompiler->newGpVar();
	  asmjit::X86GpVar v1 = JITCompiler->newGpVar();
	  pop(v0);
	  pop(v1);
	  JITCompiler->cmp(v0,v1);
	  uint32_t offset;
	  reader.Read(offset);
	  if(UALTox64Offsets.find(offset) != UALTox64Offsets.end()) {
	    JITCompiler->je(UALTox64Offsets[offset]);
	  }else {
	    asmjit::Label label = JITCompiler->newLabel();
	    pendingRelocations[offset] = label;
	    JITCompiler->je(label);
	    
	  }
	}
	break;
	case 12:
	{
	  //BNE
	  addInstruction();
	  asmjit::X86GpVar v0 = JITCompiler->newGpVar();
	  asmjit::X86GpVar v1 = JITCompiler->newGpVar();
	  pop(v0);
	  pop(v1);
	  JITCompiler->cmp(v1,v0);
	  uint32_t offset;
	  reader.Read(offset);
	  if(UALTox64Offsets.find(offset) != UALTox64Offsets.end()) {
	    JITCompiler->jne(UALTox64Offsets[offset]);
	  }else {
	    asmjit::Label label = JITCompiler->newLabel();
	    pendingRelocations[offset] = label;
	    JITCompiler->jne(label);
	    
	  }
	}
	  break;
	  case 13:
	{
	  //BGT
	  addInstruction();
	  asmjit::X86GpVar v0 = JITCompiler->newGpVar();
	  asmjit::X86GpVar v1 = JITCompiler->newGpVar();
	  pop(v0);
	  pop(v1);
	  JITCompiler->cmp(v1,v0);
	  uint32_t offset;
	  reader.Read(offset);
	  if(UALTox64Offsets.find(offset) != UALTox64Offsets.end()) {
	    JITCompiler->jg(UALTox64Offsets[offset]);
	  }else {
	    asmjit::Label label = JITCompiler->newLabel();
	    pendingRelocations[offset] = label;
	    JITCompiler->jg(label);
	    
	  }
	}
	  break;
	  case 14:
	{
	  //>=
	  addInstruction();
	  asmjit::X86GpVar v0 = JITCompiler->newGpVar();
	  asmjit::X86GpVar v1 = JITCompiler->newGpVar();
	  pop(v0);
	  pop(v1);
	  JITCompiler->cmp(v1,v0);
	  uint32_t offset;
	  reader.Read(offset);
	  if(UALTox64Offsets.find(offset) != UALTox64Offsets.end()) {
	    JITCompiler->jge(UALTox64Offsets[offset]);
	  }else {
	    asmjit::Label label = JITCompiler->newLabel();
	    pendingRelocations[offset] = label;
	    JITCompiler->jge(label);
	    
	  }
	}
	  break;
	  case 15:
	{
	  //TODO type check
	  //NOTE: We can only perform addition on native data types; not managed ones.
	  
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	   
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b); 
	    pop(output);
	     if(GetPosition()->type == ResolveType("System.Double")) {
	      
	      //NOTE: The FPU is sort of like a separate processor.
	      //The FPU has its own set of registers, and can only transfer data through the main memory bus of the chip.
	      //Therefore it is not possible to transfer values directly from the FPU to CPU registers; or vice-versa.
	      //So; unfortunately, we will have to transfer from our source registers, to memory, then to the FPU.
	      //The current optimizing engine won't be able to optimize this out, so floating point operations will be incredibly slow.
	      //This will be fixed when (and if) I add a new optimizer.
	      
	      //Temp 0, 1 = address of temporaries on stack
	      asmjit::X86GpVar temp = JITCompiler->newGpVar();
	      JITCompiler->lea(temp,stackmem);
	      JITCompiler->add(temp,stackmem_tempoffset); //Compute the address of the stack start
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp),output);
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp,8),b);
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp));
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp,8));
	      JITCompiler->fsubp(); //Eat your Raspberry Pi.
	      JITCompiler->fst(JITCompiler->intptr_ptr(temp));
	      JITCompiler->mov(output,JITCompiler->intptr_ptr(temp));
	      
	      
	     // printf("TODO: Floating point support\n");
	     // abort();
	    }else {
	      JITCompiler->sub(output,b);
	    }
	   
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	  break;
	  case 16:
	{
	  //TODO type check
	  //NOTE: We can only perform addition on native data types; not managed ones.
	  
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b); 
	    pop(output);
	     if(GetPosition()->type == ResolveType("System.Double")) {
	      
	      //NOTE: The FPU is sort of like a separate processor.
	      //The FPU has its own set of registers, and can only transfer data through the main memory bus of the chip.
	      //Therefore it is not possible to transfer values directly from the FPU to CPU registers; or vice-versa.
	      //So; unfortunately, we will have to transfer from our source registers, to memory, then to the FPU.
	      //The current optimizing engine won't be able to optimize this out, so floating point operations will be incredibly slow.
	      //This will be fixed when (and if) I add a new optimizer.
	      
	      //Temp 0, 1 = address of temporaries on stack
	      asmjit::X86GpVar temp = JITCompiler->newGpVar();
	      JITCompiler->lea(temp,stackmem);
	      JITCompiler->add(temp,stackmem_tempoffset); //Compute the address of the stack start
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp),output);
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp,8),b);
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp));
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp,8));
	      JITCompiler->fmulp(); //Eat your Raspberry Pi.
	      JITCompiler->fst(JITCompiler->intptr_ptr(temp));
	      JITCompiler->mov(output,JITCompiler->intptr_ptr(temp));
	      
	      
	     // printf("TODO: Floating point support\n");
	     // abort();
	    }else {
	      JITCompiler->imul(output,b);
	    }
	   
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	  break;
	  case 17:
	{
	  //TODO type check
	  //NOTE: We can only perform addition on native data types; not managed ones.
	  
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    asmjit::X86GpVar remainder = JITCompiler->newGpVar();
	    JITCompiler->xor_(remainder,remainder); //Rename to zero register (as per https://randomascii.wordpress.com/2012/12/29/the-surprising-subtleties-of-zeroing-a-register/)
	    pop(b); 
	    pop(output);
	    if(GetPosition()->type == ResolveType("System.Double")) {
	       asmjit::X86GpVar temp = JITCompiler->newGpVar();
	      JITCompiler->lea(temp,stackmem);
	      JITCompiler->add(temp,stackmem_tempoffset); //Compute the address of the stack start
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp),output);
	      JITCompiler->mov(JITCompiler->intptr_ptr(temp,8),b);
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp));
	      JITCompiler->fld(JITCompiler->intptr_ptr(temp,8));
	      JITCompiler->fdivp(); //NOTE: Remember the famous FDIV bug in the Pentium chip?
	      
	      JITCompiler->fst(JITCompiler->intptr_ptr(temp));
	      JITCompiler->mov(output,JITCompiler->intptr_ptr(temp));
	      
	      //abort();
	    }else {
	      JITCompiler->idiv(remainder,output,b); //Yes. The x86_64 div instruction actually works on 128-bit integers......
	    }
	  });
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	  break;
	  case 18:
	{
	  //TODO type check
	  //NOTE: We can only perform addition on native data types; not managed ones.
	  
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    asmjit::X86GpVar remainder = JITCompiler->newGpVar();
	    JITCompiler->xor_(remainder,remainder); //Rename to zero register (as per https://randomascii.wordpress.com/2012/12/29/the-surprising-subtleties-of-zeroing-a-register/)
	    pop(b); 
	    pop(output);
	    JITCompiler->idiv(remainder,output,b);
	    JITCompiler->mov(output,remainder);
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	  break;
	  
	    case 19:
	{
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b); 
	    pop(output);
	    JITCompiler->shl(output,b);
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	break;
	
	    case 20:
	{
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b); 
	    pop(output);
	    JITCompiler->shr(output,b);
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	break;  
	
	    case 21:
	{
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b); 
	    pop(output);
	    JITCompiler->and_(output,b);
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	break;  
	
	    case 22:
	{
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b); 
	    pop(output);
	    JITCompiler->or_(output,b);
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	break;  
	
	    case 23:
	{
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    asmjit::X86GpVar b = JITCompiler->newGpVar();
	    pop(b); 
	    pop(output);
	    JITCompiler->xor_(output,b);
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	break;  
	
	    case 24:
	{
	  
	  addInstruction();
	  
	  DeferredOperation* op = MakeDeferred([=](asmjit::X86GpVar output){
	    
	    pop(output);
	    JITCompiler->not_(output);
	    
	  });
	  
	  position->entryType = 4;
	  position->value = op;
	  
	  position++;
	}
	break;  
	    case 25:
	    {
	      //Load FP immediate
	      addInstruction();
	      position->entryType = 5;
	      position->type = ResolveType("System.Double");
	      double word; //We read in a doubleword
	      reader.Read(word);
	      position->value = (void*)(*(uint64_t*)&word);
	      position++;
	    }
	      break;
	
	default:
	  printf("Unknown OPCODE %i\n",(int)opcode);
	  goto velociraptor;
      }
    }
    velociraptor:
    
    if(pendingRelocations.size()) {
      printf("ERROR: Unable to compile. One or more relocated code sections could not be resolved.\n");
      
      throw "up";
    }
    
    JITCompiler->ret();
    JITCompiler->endFunc();
    
    
    nativefunc = (void(*)(GC_Array_Header*))JITCompiler->make();
    JITCompiler->make();
    //printf("Wrote %i bytes of code\n",(int)JITCompiler->getAssembler()->getCodeSize());
    //TODO: How to get size of output code?
  }
  void(*nativefunc)(GC_Array_Header*);
  
  /**
   * @summary Invokes this method with the specified arguments
   * @param args Array of arguments
   * */
  void Invoke(GC_Array_Header* arglist) {
    
    if(!isManaged) {
      Ext_Invoke(sig.methodName.data(), arglist);
      return;
      
    }
    if(nativefunc == 0) {
      Compile();
    }
    nativefunc(arglist);
    return;
    
  }
  ~UALMethod() {
    delete JITCompiler;
  }
}; 

static std::map<std::string,UALMethod*> methodCache;
class UALType:public Type {
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
  UALType() {
    //Special case: Builtin type.
    compiled = true;
  }
  /**
   * @summary Compiles this UAL type to native code (x86), or interprets
   * */
  void Compile() {
    if(!compiled) {
    uint32_t count;
    bstr.Read(count);
    size_t nativeCount = count; //Copy to size_t for faster performance
    for(size_t i = 0;i<nativeCount;i++) {
      const char* mname = bstr.ReadString();
      uint32_t mlen;
      bstr.Read(mlen);
      
      void* ptr = bstr.Increment(mlen);
      UALMethod* method = new UALMethod(BStream(ptr,mlen),module,mname);
      methods[mname] = method;
      methodCache[mname] = method;
    }
    }
  }
};


static std::map<std::string,Type*> typeCache; //Cache of types
Type* ResolveType(const char* name)
{
  return typeCache[name];
}

class UALModule {
public:
  std::map<std::string,UALType*> types;
  std::map<uint32_t,std::string> methodImports;
  
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
      UALType* type = new UALType(obj,this);
      types[std::string(name)] = type;
      typeCache[std::string(name)] = type;
      
      
    }
    str.Read(count);
    printf("Reading %i methods\n",count);
    
    for(uint32_t i = 0;i<count;i++) {
      uint32_t id;
      str.Read(id);
      char* methodName = str.ReadString();
      methodImports[id] = methodName;
      printf("Found %s\n",methodName);
    }
    
  }
  void LoadMain(int argc, char** argv) {
    //Find main
    //TODO: Parse method signatures from strings
      UALType* mainClass = 0;
      UALMethod* mainMethod = 0;
      for(auto i = types.begin();i!= types.end();i++) {
	i->second->Compile();
	for(auto bot = i->second->methods.begin();bot != i->second->methods.end();bot++) {
	  MethodSignature sig(bot->first.c_str());
	  if(sig.methodName == "Main" && sig.args.size() == 1) {
	    if(sig.args[0] == "System.String[]") {
	      mainMethod = bot->second;
	      mainClass = i->second;
	    }
	  }
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
      GC_Array_Header* argarray;
      GC_Array_Create(argarray,1);
      GC_Array_Set(argarray,0,array);
      mainMethod->Invoke(argarray);
  }
};


static UALMethod* ResolveMethod(void* assembly, uint32_t handle) {
  UALModule* module = (UALModule*)assembly;
  std::string signature = module->methodImports[handle];
  if(methodCache.find(signature) == methodCache.end()) {
    return 0;
  }
  return methodCache[signature];
}


int main(int argc, char** argv) {
  //JIT test
  /*asmjit::JitRuntime runtime;
  asmjit::X86Compiler compiler(&runtime);
  asmjit::FuncBuilderX builder;
  builder.addArg(asmjit::kVarTypeIntPtr);
  builder.addArg(asmjit::kVarTypeIntPtr);
  builder.setRet(asmjit::kVarTypeIntPtr);
  compiler.addFunc(asmjit::kFuncConvHost,builder);
  asmjit::X86GpVar a = compiler.newGpVar();
  asmjit::X86GpVar b = compiler.newGpVar();
  compiler.setArg(0,a);
  compiler.setArg(1,b);
  compiler.add(a,b);
  compiler.ret(a);
  compiler.endFunc();
  size_t(*addfunc)(size_t,size_t) = (size_t(*)(size_t,size_t))compiler.make();
  int ret = (int)addfunc(5,2);
  printf("%i\n",ret);
  return 0;
  */
  JITruntime = new asmjit::JitRuntime();
  JITCompiler = new asmjit::X86Compiler(JITruntime);
  
 /* asmjit::FuncBuilderX fbuilder;
  fbuilder.setRet(asmjit::kVarTypeIntPtr);
  JITCompiler->addFunc(asmjit::kFuncConvHost,fbuilder);
  double a = 5.2;
  double b = 3.2;
  double answer = -1;
  asmjit::X86GpVar aaddr = JITCompiler->newGpVar();
  asmjit::X86GpVar baddr = JITCompiler->newGpVar();
  asmjit::X86GpVar answeraddr = JITCompiler->newGpVar();
  
  JITCompiler->mov(aaddr,asmjit::imm((size_t)(void*)&a));
  JITCompiler->mov(baddr,(size_t)&b);
  JITCompiler->mov(answeraddr,(size_t)&answer);
  JITCompiler->fld(JITCompiler->intptr_ptr(aaddr));
  JITCompiler->fld(JITCompiler->intptr_ptr(baddr));
  JITCompiler->faddp();
  JITCompiler->fst(JITCompiler->intptr_ptr(answeraddr));
  JITCompiler->ret();
  JITCompiler->endFunc();
  void(*fptr)() = (void(*)())JITCompiler->make();
  fptr();
  
  printf("%f\n",answer);
  
  
  return 0;*/
  //Register built-ins
  abi_ext["ConsoleOut"] = (void*)ConsoleOut;
  abi_ext["PrintInt"] = (void*)PrintInt;
  abi_ext["PrintDouble"] = (void*)PrintDouble;
  
  UALType* btype = new UALType();
  btype->isStruct = true;
  btype->size = 4; //32-bit integer.
  btype->name = "System.Int32";
  typeCache["System.Int32"] = btype;
  btype = new UALType();
  btype->isStruct = false;
  btype->size = sizeof(size_t); //A String just has a single pointer.
  btype->name = "System.String";
  typeCache["System.String"] = btype;
  btype = new UALType();
  btype->isStruct = true;
  btype->size = 8;
  btype->name = "System.Double";
  typeCache["System.Double"] = btype;
  
  
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
  module->LoadMain(argc-2,argv+2);
  return 0;
}
