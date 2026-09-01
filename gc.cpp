// ================================================================
//  gc.cpp - GC::objects definition (ODR-safe)
//  This file provides the single definition for the GC static
//  member, ensuring no ODR violation across translation units.
//  Cross-platform: Windows, Linux, macOS, ARM64, RISC-V
// ================================================================
#include "value.hpp"

std::vector<GCObject*>& GC::get_objects() {
    static std::vector<GCObject*> objects;
    return objects;
}