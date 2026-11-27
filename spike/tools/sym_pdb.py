# RVA -> function and line of a module with its pdb (dbghelp), for the DUPLIX_A3D_PROF samples:
#   python spike/tools/sym_pdb.py dev_plugins/DupliX.aex 6fd2c 72cd3
import ctypes, sys
from ctypes import wintypes as W
dbg = ctypes.WinDLL("dbghelp.dll")
h = W.HANDLE(0x1234)
dbg.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES
dbg.SymInitialize(h, None, False)
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
base = dbg.SymLoadModuleEx(h, None, sys.argv[1].encode(), None, ctypes.c_uint64(0x10000000), 0x200000, None, 0)
class SYM(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", ctypes.c_uint64*2), ("Index", W.ULONG), ("Size", W.ULONG),
                ("ModBase", ctypes.c_uint64), ("Flags", W.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", W.ULONG),
                ("Scope", W.ULONG), ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG), ("Name", ctypes.c_char*512)]
class LINE(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", W.DWORD), ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]
for a in sys.argv[2:]:
    s = SYM(); s.SizeOfStruct = ctypes.sizeof(SYM) - 512 + 1; s.MaxNameLen = 500
    disp = ctypes.c_uint64(0)
    addr = base + int(a, 16)
    ok = dbg.SymFromAddr(h, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(s))
    ln = LINE(); ln.SizeOfStruct = ctypes.sizeof(LINE); d2 = W.DWORD(0)
    okl = dbg.SymGetLineFromAddr64(h, ctypes.c_uint64(addr), ctypes.byref(d2), ctypes.byref(ln))
    print(a, s.Name.decode() if ok else "?", "+%x" % disp.value, (ln.FileName.decode().split("\\")[-1] + ":" + str(ln.LineNumber)) if okl else "")
