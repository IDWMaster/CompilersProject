#include <stddef.h>

extern "C" {
  //Initializes a module of native assembly code.
  void* ASM_ModuleInit();
  //JITs the module, and returns a pointer to the function which was generated. This pointer must be casted to the appropriate function pointer type.
  void* ASM_ModuleJit();
  //Frees an ASM module
  void ASM_ModuleFree(void* module,void* compiledModule);
  //Emits a call instruction, with the specified address, arguments, and size of arguments, in bytes
  void ASM_ModuleCall(void* module, void* address, void* args, size_t len);
  //Emits a breakpoint; if supported by the host processor architecture.
  void ASM_Breakpoint(void* module);
}