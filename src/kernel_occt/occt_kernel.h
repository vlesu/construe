// Kernel interface implementation on OpenCASCADE (spec/kernel-interface.md §9).
#pragma once

#include "kernel/kernel.h"

#include <memory>
#include <string>

namespace construe::kernel {

// The OCCT kernel behind the exception boundary (§3.0а, S4-QA-004).
std::unique_ptr<Kernel> createOcctKernel();

// Exception boundary over any kernel implementation (§3.0а): exceptions of `inner` never cross the interface.
std::unique_ptr<Kernel> guardKernel(std::unique_ptr<Kernel> inner);

// Raises an OCCT exception (Standard_DomainError with the message) — for tests of the boundary without OCCT headers.
[[noreturn]] void throwOcctFailure(const std::string& message);

}  // namespace construe::kernel
