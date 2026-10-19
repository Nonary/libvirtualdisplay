#pragma once
#define check_add_overflow(a, b, result) __builtin_add_overflow(a, b, result)
#define check_mul_overflow(a, b, result) __builtin_mul_overflow(a, b, result)
