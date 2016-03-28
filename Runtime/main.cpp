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
#define GC_FAKE
#include "../GC/GC.h"
#include <set>
#include "../asmjit/src/asmjit/asmjit.h"
#define DEBUGMODE

void* gc;

//BEGIN PLATFORM CODE

asmjit::JitRuntime* JITruntime;
asmjit::X86Compiler* JITCompiler;
asmjit::X86Assembler* JITAssembler;

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
  NCallNode, NConstantInt, NConstantDouble, NConstantString, NLdLoc, NStLoc, NLdArg, NRet, NBranch, NBinaryExpression, NOPE
};



class Node {
public:
  
  NodeType type;
  std::string resultType;
  Node* next;
  Node* prev;
  bool fpEmit; //Whether or not the expression's output should be saved to the floating point stack.
  asmjit::Label label;
  bool referenced; //Whether or not this label has been referenced already
  bool bound; //Whether or not this label has been bound
  
  
  
  Node(NodeType type) {
    this->type = type;
    this->next = 0;
    this->prev = 0;
    this->fpEmit = false;
    this->bound = false;
    this->referenced = false;	
    
    //JITCompiler->nop();
    label = JITCompiler->newLabel();
    
  }
  
  virtual ~Node(){};
};

class ConstantInt:public Node {
public:
  uint32_t value;
  ConstantInt(uint32_t val):Node(NodeType::NConstantInt) {
    value = val;
    resultType = "System.Int32";
  }
};
class ConstantDouble:public Node {
public:
  double value;
  ConstantDouble(double val):Node(NodeType::NConstantDouble) {
    this->value = val;
    resultType = "System.Double";
  }
};

class ConstantString:public Node {
public:
  const char* value;
  ConstantString(const char* val):Node(NodeType::NConstantString) {
    this->value = val;
    resultType = "System.String";
  }
};

class LdLoc:public Node {
public:
  size_t idx; //The index of the local field to load from
  LdLoc(size_t idx, const char* type):Node(NodeType::NLdLoc) {
    this->idx = idx;
    this->resultType = type;
  }
};
class StLoc:public Node {
public:
  size_t idx;
  Node* exp; //The expression to store into the variable.
  StLoc(size_t idx, Node* exp):Node(NodeType::NStLoc) {
    this->idx = idx;
    this->exp = exp;
  }
};

class LdArg:public Node {
public:
  size_t index;
  LdArg(size_t index, const char* type):Node(NodeType::NLdArg) {
    this->index = index;
    this->resultType = type;
  }
};

class Ret:public Node {
public:
  Node* resultExpression;
  Ret(Node* resultExpression):Node(NRet) {
    this->resultExpression = resultExpression;
  }
};
enum BranchCondition {
  UnconditionalSurrender, //Complete and unconditional branch.
  Ble, //Barf
  Blt, //Bacon, Lettuce, and Tomato
  Bgt, //Bacon, Gravy, and Tomato.,
  Bge, //Branch on greater than or equal to
  Beq, //Branch on equal
  Bne //Branch on not equal
  
};
class Branch:public Node {
public:
  Node* left; //Democratic operand on comparison
  Node* right; //Republican operand on comparison
  uint32_t offset; //Byte offset from start of executable segment of method.
  BranchCondition condition;
  Branch(uint32_t ualoffset, BranchCondition condition, Node* democrat, Node* republican):Node(NBranch) {
    offset = ualoffset;
    this->condition = condition;
    left = democrat;
    right = republican;
  }
};



//Objects are referred to when possible by reference identifier strings
//to allow for dynamic modules to be loaded and unloaded without requiring recompilation of existing code,
//and to allow for recompilation of a method if necessary for whatever reason.
class CallNode:public Node {
public:
  UALMethod* method;
  std::vector<Node*> arguments;
  CallNode(UALMethod* method, const std::vector<Node*>& arguments):Node(NodeType::NCallNode) {
    this->method = method;
    this->arguments = arguments;
  }
  
};


class BinaryExpression:public Node {
public:
  char op;
  //OPS
  //+ -- Addition
  //- -- Subtraction
  //* -- Multiplication
  /// -- Division
  //% -- Reminder
  //< -- Shift left
  //> -- Shift right
  //& -- AND
  //| -- OR
  //! -- NOT (only needs Democratic debate)
  //~ -- XOR
  Node* left;
  Node* right;
  BinaryExpression(char op, Node* democrat, Node* republican):Node(NBinaryExpression) {
    this->op = op;
    left = democrat;
    right = republican;
    this->resultType = left->resultType;
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
  //asmjit::X86Compiler* JITCompiler;
  
  //BEGIN Optimization engine:
  std::vector<Node*> nodes;
    std::vector<Node*> stack;
  Node* instructions;
  Node* lastInstruction;
  std::map<uint32_t,Node*> ualOffsets;
  asmjit::X86GpVar* arg_regs;
  template<typename T, typename... arg>
  //Adds an Instruction node to the tree
  T* Node_Instruction(arg... uments) {
    T* retval = new T(uments...);
    nodes.push_back(retval);
    if(instructions == 0) {
      instructions = retval;
      lastInstruction = retval;
    }else {
      lastInstruction->next = retval;
      retval->prev = lastInstruction;
      lastInstruction = retval;
    }
    if(ualOffsets.find(this->ualip) != ualOffsets.end()) {
      printf("ERROR: Insanity.\n");
      throw "sideways";
    }
    ualOffsets[this->ualip] = retval;
    return retval;
  }
  template<typename T, typename... arg>
  T* Node_Stackop(arg... uments) {
    T* retval = new T(uments...);
    nodes.push_back(retval);
    stack.push_back(retval);
    
    if(ualOffsets.find(this->ualip) != ualOffsets.end()) {
      printf("ERROR: Insanity.\n");
      throw "sideways";
    }
    ualOffsets[this->ualip] = retval;
    return retval;
  }
  template<typename T>
  //Removes an instruction node
  T* Node_RemoveInstruction(T* node) {
    if(node->prev) {
      node->prev->next = node->next;
    }
    if(node->next) {
      node->next->prev = node->prev;
    }
    if(node == instructions) {
      instructions = node->next;
    }
    if(node == lastInstruction) {
      lastInstruction = node->prev;
    }
    
    node->next = 0;
    node->prev = 0;
    return node;
  }
  //END Optimization engine
  
  UALMethod(const BStream& str, void* assembly, const char* sig) {
    this->funcStart = JITCompiler->newLabel();
    this->instructions = 0;
   // this->JITCompiler = new asmjit::X86Compiler(JITruntime);
    this->sig = sig;
    this->str = str;
    this->str.Read(isManaged);
    arg_regs = new asmjit::X86GpVar[this->sig.args.size()];
    if(isManaged) {
      this->str.Read(localVarCount);
      locals.resize(localVarCount);
      for(size_t i = 0;i<localVarCount;i++) {
	locals[i] = this->str.ReadString();
      }
      
    }
    this->assembly = assembly;
    nativefunc = 0;
    constantStrings = 0;
    stringCount = 0;
    stringCapacity = 0;
  }
  
  
  GC_String_Header** constantStrings;
  void* constaddr;
  size_t stringCount;
  size_t stringCapacity;
  uint32_t ualip; //Instruction pointer into UAL
  void EnsureCapacity() {
    if(stringCount == stringCapacity) {
      GC_String_Header** newList = new GC_String_Header*[stringCapacity*2];
      for(size_t i = 0;i<stringCount;i++) {
	newList[i] = constantStrings[i];
	GC_Unmark((void**)(constantStrings+i),true);
	GC_Mark((void**)(newList+i),true);
      }
      delete[] constantStrings;
      constantStrings = newList;
      stringCapacity*=2;
    }
  }
  std::map<std::string,size_t> constantMappings;
  
  size_t GetString(const char* str) {
    if(constantMappings.find(str) != constantMappings.end()) {
      return constantMappings[str];
    }
    if(constantStrings == 0) {
      constantStrings = new GC_String_Header*[1];
      GC_String_Create(constantStrings[0],str);
      GC_Mark((void**)constantStrings,true);
      stringCapacity = 1;
    }else {
      EnsureCapacity();
      GC_String_Create(constantStrings[stringCount],str);
      GC_Mark((void**)constantStrings+stringCount,true);
    }
    constantMappings[str] = stringCount;
    stringCount++;
    constaddr = constantStrings;
    return stringCount-1;
  }
  
  void Optimize() {
  }
  asmjit::X86Mem stackmem;
  size_t* stackOffsetTable;
  size_t stackSize;
  //Internal -- Emits x86 code for a MARK instruction given a specified register containing a memory address to mark
  void EmitMark(asmjit::X86GpVar memreg, bool isRoot) {
    asmjit::FuncBuilderX builder;
    builder.addArg(asmjit::kVarTypeIntPtr);
    builder.addArg(asmjit::kVarTypeIntPtr);
    asmjit::X86CallNode* call = JITCompiler->call((size_t)&GC_Mark,builder);
    call->setArg(0,memreg);
    call->setArg(1,asmjit::imm(isRoot));
  }
  void EmitUnmark(asmjit::X86GpVar memreg, bool isRoot) {
    asmjit::FuncBuilderX builder;
    builder.addArg(asmjit::kVarTypeIntPtr);
    builder.addArg(asmjit::kVarTypeIntPtr);
    asmjit::X86CallNode* call = JITCompiler->call((size_t)&GC_Unmark,builder);
    call->setArg(0,memreg);
    call->setArg(1,asmjit::imm(isRoot));
  }
  
  asmjit::HLNode* currentNode;
  
  
  //Internal -- Emits x86 code for a given tree node.
  void EmitNode(Node* inst, asmjit::X86GpVar output) {
    if(inst->bound) {
      abort();
    }
      JITCompiler->bind(inst->label);
      inst->bound = true;
    
    
    switch(inst->type) {
	case NStLoc:
	{
	  StLoc* op = (StLoc*)inst;
	  if(op->exp->resultType == "System.Double") {
	    //Optimize for double
	    op->exp->fpEmit = true;
	    EmitNode(op->exp,output);
	    
	    if(op->exp->fpEmit) {
	      printf("BUG DETECTED: Subtree did not emit floating point values to stack (or fpEmit flag not cleared).\n");
	      abort();
	    }
	    asmjit::X86GpVar addr = JITCompiler->newIntPtr();
	  JITCompiler->lea(addr,stackmem);
	  
	    //Write floating point to memory
	    JITCompiler->fstp(JITCompiler->intptr_ptr(addr,(int32_t)stackOffsetTable[op->idx]));
	  }else {
	  //Store result of expression into local variable
	  asmjit::X86GpVar temp = JITCompiler->newIntPtr();
	  EmitNode(op->exp,temp);
	  asmjit::X86GpVar addr = JITCompiler->newIntPtr();
	  JITCompiler->lea(addr,stackmem);
	  JITCompiler->mov(JITCompiler->intptr_ptr(addr,(int32_t)stackOffsetTable[op->idx]),temp);
	  if(!ResolveType(op->exp->resultType.data())->isStruct) {
	    JITCompiler->add(addr,(int32_t)stackOffsetTable[op->idx]);
	    EmitMark(addr,true);
	  }
	  }
	}
	  break;
	  
	    case NLdLoc:
	    {
	      //TODO: Load local variable
	      LdLoc* op = (LdLoc*)inst;
	      asmjit::X86GpVar addr = JITCompiler->newIntPtr();
	      JITCompiler->lea(addr,stackmem); //Load the effective base address of the stack
	      if(op->fpEmit) {
		//Store value into FPU
		JITCompiler->fld(JITCompiler->intptr_ptr(addr,(int32_t)stackOffsetTable[op->idx]));
		//Clear floating point flag
		op->fpEmit = false;
	      }else {
		//Load the value from the base address into the output register
		JITCompiler->mov(output,JITCompiler->intptr_ptr(addr,(int32_t)stackOffsetTable[op->idx])); 
	      }
	    }
	      break;
	    case NLdArg:
	    {
	      LdArg* op = (LdArg*)inst;
	      if(op->fpEmit) {
		printf("TODO: Floating point arguments\n");
		abort();
	      }else {
		//Load the value from the base address into the output register
		JITCompiler->mov(output,arg_regs[op->index]); 
	      }
	    }
	      break;
	case NConstantString:
	{
	  //Load constant string (we can now do this with only 2 instructions! Thanks to the constant pool.)
	  asmjit::X86GpVar temp = JITCompiler->newIntPtr();
	  //Load base address of constant pool
	  JITCompiler->mov(temp,asmjit::imm((size_t)&constaddr));
	  //Get memory address of constant pool (array)
	  JITCompiler->mov(temp,JITCompiler->intptr_ptr(temp));
	  
	  //Return absolute memory address of string (by dereferencing the index in the array)
	  JITCompiler->mov(output,JITCompiler->intptr_ptr(temp,sizeof(size_t)*GetString(((ConstantString*)inst)->value)));
	  
	}
	  break;
	case NCallNode:
	{
	  //Function call
	  CallNode* callme = (CallNode*)inst;
	  asmjit::FuncBuilderX builder;
	  for(size_t i = 0;i<callme->arguments.size();i++) {
	    builder.addArg(asmjit::kVarTypeIntPtr);
	  }
	  
	  UALMethod* method = callme->method;
	  if(method->sig.returnType != "System.Void") {
	    builder.setRet(asmjit::kVarTypeIntPtr);
	  }
	  asmjit::X86GpVar* realargs = new asmjit::X86GpVar[method->sig.args.size()]; //varargs
	  for(size_t i = 0;i<method->sig.args.size();i++) {
	    realargs[i] = JITCompiler->newIntPtr();
	    EmitNode(callme->arguments[i],realargs[i]);
	    
	  }
	  
	  asmjit::X86CallNode* call;
	  if(callme->method->isManaged) {
	   // printf("Managed method %s\n",method->sig.methodName.data());
	    call = JITCompiler->call(method->funcStart,builder);
	    if(callme->method->sig.returnType != "System.Void") {
	      call->setRet(0,output);
	    }
	  }else {
	    call = JITCompiler->call((size_t)abi_ext[method->sig.methodName],builder);
	  }
	  //Bind arguments
	  for(size_t i = 0;i<callme->arguments.size();i++) {
	    call->setArg(i,realargs[i]);
	  }
	  delete[] realargs;
	}
	  break;
	case NConstantInt:
	{
	  ConstantInt* ci = (ConstantInt*)inst;
	  JITCompiler->mov(output,asmjit::imm(ci->value));
	}
	  break;
	case NConstantDouble:
	{
	  ConstantDouble* cv = (ConstantDouble*)inst;
	  //Load constant double
	  uint64_t val = *(uint64_t*)&cv->value;
	  if(cv->fpEmit) {
	    cv->fpEmit = false;
	    //NOTE: Assume instruction nodes stay constant in memory throughout program execution.
	    asmjit::X86GpVar addr = JITCompiler->newIntPtr();
	    JITCompiler->mov(addr,asmjit::imm((size_t)&cv->value));
	    JITCompiler->fld(JITCompiler->intptr_ptr(addr));
	    
	  }else {
	    JITCompiler->mov(output,asmjit::imm(val));
	  }
	}
	  break;
	case NBinaryExpression:
	{
	  BinaryExpression* binexp = (BinaryExpression*)inst;
	  switch(binexp->op) {
	    case '+':
	    {
	      if(binexp->left->resultType == "System.Double") { //If we're a double, use the floating point unit instead of the processor.
		binexp->left->fpEmit = true;
		binexp->right->fpEmit = true;
		EmitNode(binexp->right,output); //Evaluate Democratic candidates.
		EmitNode(binexp->left,output); //Evaluate Republican candidates.
		
		if(binexp->left->fpEmit || binexp->right->fpEmit) {
		  printf("BUG DETECTED: Subtree did not emit floating point values to stack (or fpEmit flag not cleared).\n");
		  abort();
		}
		JITCompiler->faddp();
		if(binexp->fpEmit) {
		  binexp->fpEmit = false; //Let caller know that we've handled floating-point value.
		}else {
		  //Write value to output register
		  asmjit::X86GpVar addr = JITCompiler->newIntPtr();
		  JITCompiler->lea(addr,stackmem);
		  JITCompiler->fstp(JITCompiler->intptr_ptr(addr,stackSize));
		  JITCompiler->mov(output,JITCompiler->intptr_ptr(addr,stackSize));
		}
	      }else {
		//Use the ALU on the CPU rather than the FPU.
		asmjit::X86GpVar r = JITCompiler->newIntPtr();
		EmitNode(binexp->left,r);
		EmitNode(binexp->right,output);
		JITCompiler->add(output,r);
	      }
	    }
	      break;
	      case '-':
	    {
	      if(binexp->left->resultType == "System.Double") { //If we're a double, use the floating point unit instead of the processor.
		binexp->left->fpEmit = true;
		binexp->right->fpEmit = true;
		EmitNode(binexp->right,output); //Evaluate Democratic candidates.
		EmitNode(binexp->left,output); //Evaluate Republican candidates.
		
		if(binexp->left->fpEmit || binexp->right->fpEmit) {
		  printf("BUG DETECTED: Subtree did not emit floating point values to stack (or fpEmit flag not cleared).\n");
		  abort();
		}
		JITCompiler->fsubp();
		if(binexp->fpEmit) {
		  binexp->fpEmit = false; //Let caller know that we've handled floating-point value.
		}else {
		  //Write value to output register
		  asmjit::X86GpVar addr = JITCompiler->newIntPtr();
		  JITCompiler->lea(addr,stackmem);
		  JITCompiler->fstp(JITCompiler->intptr_ptr(addr,stackSize));
		  JITCompiler->mov(output,JITCompiler->intptr_ptr(addr,stackSize));
		}
	      }else {
		//Use the ALU on the CPU rather than the FPU.
		asmjit::X86GpVar r = JITCompiler->newIntPtr();
		
		
		EmitNode(binexp->left,r);
		JITCompiler->int3();
		JITCompiler->nop();
		JITCompiler->nop();
		EmitNode(binexp->right,output);
		
		JITCompiler->sub(output,r);
	      }
	    }
	      break;
	      case '*':
	    {
	      if(binexp->left->resultType == "System.Double") { //If we're a double, use the floating point unit instead of the processor.
		binexp->left->fpEmit = true;
		binexp->right->fpEmit = true;
		EmitNode(binexp->right,output); //Evaluate Democratic candidates.
		EmitNode(binexp->left,output); //Evaluate Republican candidates.
		
		if(binexp->left->fpEmit || binexp->right->fpEmit) {
		  printf("BUG DETECTED: Subtree did not emit floating point values to stack (or fpEmit flag not cleared).\n");
		  abort();
		}
		JITCompiler->fmulp();
		if(binexp->fpEmit) {
		  binexp->fpEmit = false; //Let caller know that we've handled floating-point value.
		}else {
		  //Write value to output register
		  asmjit::X86GpVar addr = JITCompiler->newIntPtr();
		  JITCompiler->lea(addr,stackmem);
		  JITCompiler->fstp(JITCompiler->intptr_ptr(addr,stackSize));
		  JITCompiler->mov(output,JITCompiler->intptr_ptr(addr,stackSize));
		}
	      }else {
		//Use the ALU on the CPU rather than the FPU.
		asmjit::X86GpVar r = JITCompiler->newIntPtr();
		EmitNode(binexp->left,r);
		EmitNode(binexp->right,output);
		JITCompiler->imul(output,r);
	      }
	    }
	      break;
	      case '/':
	    {
	      if(binexp->left->resultType == "System.Double") { //If we're a double, use the floating point unit instead of the processor.
		binexp->left->fpEmit = true;
		binexp->right->fpEmit = true;
		EmitNode(binexp->right,output); //Evaluate Democratic candidates.
		EmitNode(binexp->left,output); //Evaluate Republican candidates.
		
		if(binexp->left->fpEmit || binexp->right->fpEmit) {
		  printf("BUG DETECTED: Subtree did not emit floating point values to stack (or fpEmit flag not cleared).\n");
		  abort();
		}
		JITCompiler->fdivp();
		if(binexp->fpEmit) {
		  binexp->fpEmit = false; //Let caller know that we've handled floating-point value.
		}else {
		  //Write value to output register
		  asmjit::X86GpVar addr = JITCompiler->newIntPtr();
		  JITCompiler->lea(addr,stackmem);
		  JITCompiler->fstp(JITCompiler->intptr_ptr(addr,stackSize));
		  JITCompiler->mov(output,JITCompiler->intptr_ptr(addr,stackSize));
		}
	      }else {
		//Use the ALU on the CPU rather than the FPU.
		asmjit::X86GpVar r = JITCompiler->newIntPtr();
		EmitNode(binexp->left,r);
		EmitNode(binexp->right,output);
		asmjit::X86GpVar reminder = JITCompiler->newIntPtr();
		JITCompiler->xor_(reminder,reminder);
		JITCompiler->idiv(reminder,output,r);
	      }
	    }
	      break;
	      case '%':
	    {
	     //Use the ALU on the CPU rather than the FPU.
		asmjit::X86GpVar r = JITCompiler->newIntPtr();
		EmitNode(binexp->left,r);
		EmitNode(binexp->right,output);
		asmjit::X86GpVar reminder = JITCompiler->newIntPtr();
		JITCompiler->xor_(reminder,reminder);
		JITCompiler->idiv(reminder,output,r);
		JITCompiler->mov(output,reminder);
	    }
	      break;
	    default:
	      printf("Operator %c not implemented yet....\n",binexp->op);
	      abort();
	  }
	}
	  break;
	    case NBranch:
	    {
	      Branch* b = (Branch*)inst;
	      if(ualOffsets.find(b->offset) == ualOffsets.end()) {
		throw "Illegal UAL offset";
	      }
	      Node* bnode = this->ualOffsets[b->offset]; //Node to branch to
	      switch(b->condition) {
		case UnconditionalSurrender:
		{
		  
		  JITCompiler->jmp(bnode->label);
		}
		  break;
		case Ble:
		{
		  //Check conditions
		  asmjit::X86GpVar left = JITCompiler->newIntPtr();
		  asmjit::X86GpVar right = JITCompiler->newIntPtr();
		  
		 //ldconst.12 -- Load constant 12, this seems to be ommitted for some reason.
		  EmitNode(b->right,right);
		  EmitNode(b->left,left);
		  JITCompiler->cmp(right,left);
		  JITCompiler->jle(bnode->label);
		}
		  break;
		  case Beq:
		{
		  //Check conditions
		  asmjit::X86GpVar left = JITCompiler->newIntPtr();
		  asmjit::X86GpVar right = JITCompiler->newIntPtr();
		  
		 //ldconst.12 -- Load constant 12, this seems to be ommitted for some reason.
		  EmitNode(b->right,right);
		  EmitNode(b->left,left);
		  JITCompiler->cmp(right,left);
		  JITCompiler->je(bnode->label);
		}
		  break;
		  case Blt:
		{
		  //Check conditions
		  asmjit::X86GpVar left = JITCompiler->newIntPtr();
		  asmjit::X86GpVar right = JITCompiler->newIntPtr();
		  
		 //ldconst.12 -- Load constant 12, this seems to be ommitted for some reason.
		  EmitNode(b->right,right);
		  EmitNode(b->left,left);
		  JITCompiler->cmp(right,left);
		  JITCompiler->jl(bnode->label);
		}
		  break;
		  case Bgt:
		{
		  //Check conditions
		  asmjit::X86GpVar left = JITCompiler->newIntPtr();
		  asmjit::X86GpVar right = JITCompiler->newIntPtr();
		  
		 //ldconst.12 -- Load constant 12, this seems to be ommitted for some reason.
		  EmitNode(b->right,right);
		  EmitNode(b->left,left);
		  JITCompiler->cmp(right,left);
		  JITCompiler->jg(bnode->label);
		}
		  break;
		  case Bge:
		{
		  //Check conditions
		  asmjit::X86GpVar left = JITCompiler->newIntPtr();
		  asmjit::X86GpVar right = JITCompiler->newIntPtr();
		  
		 //ldconst.12 -- Load constant 12, this seems to be ommitted for some reason.
		  EmitNode(b->right,right);
		  EmitNode(b->left,left);
		  JITCompiler->cmp(right,left);
		  JITCompiler->jge(bnode->label);
		}
		  break;
		  case Bne:
		{
		  //Check conditions
		  asmjit::X86GpVar left = JITCompiler->newIntPtr();
		  asmjit::X86GpVar right = JITCompiler->newIntPtr();
		  
		 //ldconst.12 -- Load constant 12, this seems to be ommitted for some reason.
		  EmitNode(b->right,right);
		  EmitNode(b->left,left);
		  JITCompiler->cmp(right,left);
		  JITCompiler->jne(bnode->label);
		}
		  break;
		  
		default:
		printf("TODO: Implement branch\n");
		abort();
	      }
	      
	    }
	      break;
		case NOPE:
		  //Not gonna do that
		  break;
		case NRet:
		{
		  Ret* val = (Ret*)inst;
		  if(val->resultExpression) {
		    
		  asmjit::X86GpVar retreg = JITCompiler->newIntPtr();
		  
		    EmitNode(val->resultExpression,retreg);
		    JITCompiler->ret(retreg);
		  }else {
		    JITCompiler->ret();
		  }
		  
		}
		  break;
	default:
	  printf("Unknown tree instruction.\n");
	  abort();
      }
  }
asmjit::Label funcStart;
  void Emit() {
    currentNode = 0;
    asmjit::FuncBuilderX builder;
    if(sig.returnType != "System.Void") {
      builder.setRet(asmjit::kVarTypeIntPtr);
    }
    for(size_t i = 0;i<this->sig.args.size();i++) {
      builder.addArg(asmjit::kVarTypeIntPtr);
    }
    JITCompiler->bind(funcStart);
    asmjit::X86FuncNode* fnode = JITCompiler->addFunc(builder);
    for(size_t i = 0;i<sig.args.size();i++) {
      arg_regs[i] = JITCompiler->newIntPtr();
      JITCompiler->setArg(i,arg_regs[i]); //TODO: Something here with args causes assertion failure about register ID.
    }
    //BEGIN set up stack
    
    stackOffsetTable = new size_t[localVarCount];
    stackSize = 0;
    {
      size_t cOffset = 0;
      for(size_t i = 0;i<localVarCount;i++) {
	Type* tdef = ResolveType(this->locals[i].data());
	size_t requiredSize = 0;
	if(tdef->isStruct) {
	  requiredSize = tdef->size;
	}else {
	  requiredSize = sizeof(size_t);
	}
	requiredSize+=(requiredSize % 8); //Align stack to largest size possible primitive datatype.
	
	stackSize+=requiredSize;
	stackOffsetTable[i] = cOffset;
	cOffset+=requiredSize;
      }
    }
    stackmem = JITCompiler->newStack(stackSize+(sizeof(double)*2),8);
    //END set up stack
    //BEGIN VARIABLES
    
    //END VARIABLES
    
    
    //BEGIN code emit
    
    asmjit::X86GpVar output = JITCompiler->newIntPtr();
    for(Node* inst = instructions;inst != 0;inst = inst->next) {
      
      EmitNode(inst,output);
    }
    //END Code emit
    JITCompiler->endFunc();
    
  }
  void Compile() {
    Parse();
    Optimize();
    Emit();
  }
  
  
  void Parse() {
    
    //Generate parse tree
    unsigned char opcode;
    unsigned char* base = str.ptr;
    BStream reader = str;
    while(reader.Read(opcode) != 255) {
      ualip = (uint32_t)((size_t)reader.ptr-(size_t)base)-1;
      
      printf("OPCODE: %i\n",(int)opcode);
      switch(opcode) {
	case 0:
	{
	  //Push argument to evaluation stack
	  uint32_t index;
	  reader.Read(index);
	  std::string tname = sig.args[index];
	  Node* sobj = Node_Stackop<LdArg>(index,tname.data());
	  
	  
	}
	  break;
	case 1:
	  //Call function
	{
	  uint32_t funcID;
	  reader.Read(funcID);
	  UALMethod* method = ResolveMethod(assembly,funcID);
	  size_t argcount = method->sig.args.size();
	  std::vector<Node*> args;
	  args.resize(argcount);
	  for(size_t i = 0;i<argcount;i++) {
	    if(stack.size() == 0) {
	      throw "Malformed UAL. Too few arguments in function call.";
	    }
	    args[argcount-i-1] = stack[stack.size()-1];
	    if(args[argcount-i-1]->resultType != method->sig.args[argcount-i-1]) {
	      throw "Malformed UAL. Illegal data type passed to function.";
	    }
	    stack.pop_back();
	    Node_RemoveInstruction(args[argcount-i-1]);
	  }
	  Node* sobj = Node_Instruction<CallNode>(method,args);
	  if(method->sig.returnType != "System.Void") {
	    sobj->resultType = method->sig.returnType;
	    stack.push_back(sobj); //Hybrid instruction
	  }
	  
	}
	  break;
	case 2:
	  //Load string
	{
	  Node* sobj = Node_Stackop<ConstantString>(reader.ReadString());
	  
	}
	  break;
	case 3:
	{
	  if(this->sig.returnType == "System.Void") {
	    Node_Instruction<Ret>((Node*)0);
	    //There should be nothing on stack
	    if(stack.size()) {
	      throw "Malformed UAL. Function should not return a value.";
	    }
	  }else {
	      if(stack.size() != 1) {
		throw "Malformed UAL. Function must return a value.";
	      }
	      if(stack[0]->resultType != this->sig.returnType) {
		throw "Malformed UAL. Function does not return correct datatype.";
	      }
	      Node_Instruction<Ret>(Node_RemoveInstruction(stack[0]));
	      stack.pop_back();
	    }
	}
	  break;
	case 4:
	{
	  //Load 32-bit integer immediate
	  uint32_t val;
	  reader.Read(val);
	  Node_Stackop<ConstantInt>(val);
	}
	  break;
	case 5:
	  //stloc
	{
	  uint32_t index;
	  reader.Read(index);
	  
	  //Store value to local
	  if(stack.size() == 0) {
	    throw "Malformed UAL. Expected value on stack.";
	  }
	  Node* sval = stack[stack.size()-1];
	  stack.pop_back();
	  if(sval->resultType != this->locals[index]) {
	    printf("Type mismatch %s != %s\n",sval->resultType.data(),this->locals[index].data());
	    throw "Malformed UAL. Type mismatch on store to local variable.";
	  }
	  Node_Instruction<StLoc>(index,Node_RemoveInstruction(sval));
	  
	  
	  
	}
	  break;
	case 6:
	  //Branch to str.ptr+offset
	{
	  uint32_t offset;
	  reader.Read(offset);
	   Node_Instruction<Branch>(offset, UnconditionalSurrender, (Node*)0, (Node*)0);
	   
	}
	  break;
	case 7:
	{
	  //LDLOC
	  uint32_t id;
	  reader.Read(id);
	  std::string tname = this->locals[id];
	  Node_Stackop<LdLoc>(id,tname.data());
	}
	  break;
	case 8:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('+',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	case 9:
	{
	  //BLE!!!!!! (NOTE: emit barfing sound here into the assembly code)
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Ble,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	  break;
	case 10:
	  //NOPE. Not gonna happen.
	{
	  Node_Instruction<Node>(NOPE);
	}
	  break;
	case 11:
	  {
	  //BEQ
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Beq,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	}
	break;
	case 12:
	{
	  //BNE
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Bne,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	}
	  break;
	  case 13:
	{
	  //BGT
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Bgt,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	  case 14:
	{
	  //>=
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Bge,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	}
	  break;
	  case 15:
	{
	  
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('-',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	  
	}
	  break;
	  case 16:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('*',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	  break;
	  case 17:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('/',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	  case 18:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('%',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	  
	    case 19:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('<',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	  
	  
	}
	break;
	
	    case 20:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('>',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	break;  
	
	    case 21:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('&',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	break;  
	
	    case 22:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('|',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	break;  
	
	    case 23:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('~',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	break;  
	
	    case 24:
	{
	  
	  if(stack.size() < 1) {
	    throw "Malformed UAL. Expected at least one operand on the stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('!',Node_RemoveInstruction(left),(Node*)0);
	  
	  
	}
	break;  
	    case 25:
	    {
	      //Load FP immediate
	      double word; //We read in a doubleword
	      reader.Read(word);
	      Node_Stackop<ConstantDouble>(word);
	      
	    }
	      break;
	
	default:
	  printf("Unknown OPCODE %i\n",(int)opcode);
	  goto velociraptor;
      }
    }
    
    velociraptor: //Back pain? Visit your GOTO Velociraptor today!
    return;
    
  }
  void* nativefunc;
  
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
      throw "up";
    }
    ((void(*)(void*))nativefunc)(arglist);
    return;
    
  }
  ~UALMethod() {
   // delete JITCompiler;
    size_t l = nodes.size();
    for(size_t i = 0;i<l;i++) {
      delete nodes[i];
    }
    
    for(size_t i = 0;i<stringCount;i++) {
      GC_Unmark((void**)(constantStrings+i),true);
    }
    delete[] constantStrings;
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
      
      method->sig = mname;
      methods[mname] = method;
      methodCache[mname] = method;
      
      if(method->isManaged) {
	method->Compile();
	
      }
      
    }
    
    
      JITCompiler->finalize();
      size_t start = (size_t)JITAssembler->make();
      for(auto i = methods.begin();i != methods.end();i++) {
	UALMethod* meth = i->second;
	meth->nativefunc = (void*)(start+JITAssembler->getLabelOffset(meth->funcStart));
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
  asmjit::X86GpVar a = compiler.newIntPtr();
  asmjit::X86GpVar b = compiler.newIntPtr();
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
  JITAssembler = new asmjit::X86Assembler(JITruntime);
  JITCompiler = new asmjit::X86Compiler(JITAssembler);
  
  
  
  asmjit::FileLogger logger(stdout);
  JITAssembler->setLogger(&logger);
  
  
  
 /* asmjit::FuncBuilderX fbuilder;
  fbuilder.setRet(asmjit::kVarTypeIntPtr);
  JITCompiler->addFunc(asmjit::kFuncConvHost,fbuilder);
  double a = 5.2;
  double b = 3.2;
  double answer = -1;
  asmjit::X86GpVar aaddr = JITCompiler->newIntPtr();
  asmjit::X86GpVar baddr = JITCompiler->newIntPtr();
  asmjit::X86GpVar answeraddr = JITCompiler->newIntPtr();
  
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
