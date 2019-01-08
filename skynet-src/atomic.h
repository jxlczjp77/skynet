#ifndef SKYNET_ATOMIC_H
#define SKYNET_ATOMIC_H

#ifdef _MSC_VER
#include <windows.h>
#define ATOM_CAS(ptr, oval, nval) (InterlockedCompareExchange(ptr, nval, oval) == (oval))
#define ATOM_CAS16(ptr, oval, nval) (InterlockedCompareExchange16(ptr, nval, oval) == (oval))
#define ATOM_CAS_POINTER(ptr, oval, nval) InterlockedCompareExchangePointer(ptr, nval, oval)
#define ATOM_INC(ptr) InterlockedIncrement(ptr)
#define ATOM_INC16(ptr) InterlockedIncrement16(ptr)
#define ATOM_FINC(ptr) InterlockedIncrement(ptr)
#define ATOM_DEC(ptr) InterlockedDecrement(ptr)
#define ATOM_DEC16(ptr) InterlockedDecrement16(ptr)
#define ATOM_FDEC(ptr) InterlockedDecrement(ptr)
#define ATOM_ADD(ptr,n) InterlockedExchangeAdd(ptr, n)
#define ATOM_SUB(ptr,n) InterlockedExchangeAdd(ptr, -(n))
#define ATOM_AND(ptr,n) InterlockedAnd(ptr, n)
#else
#define ATOM_CAS(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_CAS_POINTER(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_INC(ptr) __sync_add_and_fetch(ptr, 1)
#define ATOM_FINC(ptr) __sync_fetch_and_add(ptr, 1)
#define ATOM_DEC(ptr) __sync_sub_and_fetch(ptr, 1)
#define ATOM_FDEC(ptr) __sync_fetch_and_sub(ptr, 1)
#define ATOM_ADD(ptr,n) __sync_add_and_fetch(ptr, n)
#define ATOM_SUB(ptr,n) __sync_sub_and_fetch(ptr, n)
#define ATOM_AND(ptr,n) __sync_and_and_fetch(ptr, n)
#endif

#endif
