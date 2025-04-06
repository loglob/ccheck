#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

/** The type of a provider function */
typedef size_t (*provider_f)(size_t, void*);
/** The type of a formatter function */
typedef size_t (*format_f)(char*, size_t, const void*);

/** The maximum number of function arguments allowed for TEST() functions  */
#define MAX_ARITY 8

#define _MANY_ARGS( _0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, N, ... ) N
#define MANY_ARGS(...) _MANY_ARGS( __VA_ARGS__ , 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0 )

#define _STR(x) #x
#define STR(x) _STR(x)
#define SSTR(x) STR(x)

#define _CAT(x, y) x##y
#define CAT(x, y) _CAT(x, y)
#define CCAT(x, y) CAT(x, y)

#define EVAL(x) x
#define EVAL_4(x) EVAL(EVAL(EVAL(EVAL(x))))
#define EVAL_16(x) EVAL_4(EVAL_4(EVAL_4(EVAL_4(x))))
#define LATER()
#define RECURSE(pfx, ...) CAT(pfx, MANY_ARGS(__VA_ARGS__)) LATER() (__VA_ARGS__)

#define _JOIN_2(x, y, ...) #x "\0" #y "\0" _JOIN LATER() (__VA_ARGS__)
#define _JOIN_1(x, y) #x "\0" #y
#define _JOIN_0(x) #x
#define _JOIN(...) RECURSE(_JOIN_ ,##__VA_ARGS__)
/** Stringifies a list of argument and joins them with NUL separators. Terminates the list with an empty entry. */
#define JOIN(...) EVAL_16(_JOIN(__VA_ARGS__)) "\0\0"

#define _PAIR_2(x, y, ...) x y , _PAIR LATER() (__VA_ARGS__)
#define _PAIR_1(x, y) x y
#define _PAIR_0()
#define _PAIR(...) RECURSE(_PAIR_ ,##__VA_ARGS__)
/** Removes every second comma, starting with the first one. */
#define PAIR(...) EVAL_16((_PAIR(__VA_ARGS__)))

#define _UNCOMMA_2(x, y, ...) x y _UNCOMMA LATER() (__VA_ARGS__)
#define _UNCOMMA_1(x, y) x y
#define _UNCOMMA_0(x) x
#define _UNCOMMA(...) RECURSE(_UNCOMMA_ ,##__VA_ARGS__)
/** Removes every comma */
#define UNCOMMA(...) EVAL_16(_UNCOMMA(__VA_ARGS__))

#define _UNSEP_2(x, y, ...) x##_##y ,##__VA_ARGS__
#define _UNSEP_1(x, y) x##_##y
#define _UNSEP_0(x) x
#define __UNSEP(...) CAT(_UNSEP_, MANY_ARGS(__VA_ARGS__))(__VA_ARGS__)
#define _UNSEP(...) __UNSEP(__UNSEP(__UNSEP(__UNSEP(__VA_ARGS__))))
/** Replaces commas with '_', swallowing whitespace */
#define UNSEP(...) _UNSEP(_UNSEP(_UNSEP(_UNSEP(__VA_ARGS__))))

#define _PTR_ARG_2(x, y, ...) const x *y , _PTR_ARGS LATER() (__VA_ARGS__)
#define _PTR_ARG_1(x, y) const x *y
#define _PTR_ARG_0()
#define _PTR_ARGS(...) RECURSE(_PTR_ARG_ ,##__VA_ARGS__)
/** interprets every pair of items as a type and variable, and turns the type into a constant pointer */
#define PTR_ARGS(...) EVAL_16((_PTR_ARGS(__VA_ARGS__)))

#define _INVOKE_PTR_ARG_2(x, y, ...) *y , _INVOKE_PTR_ARGS LATER() (__VA_ARGS__)
#define _INVOKE_PTR_ARG_1(x, y) *y
#define _INVOKE_PTR_ARG_0()
#define _INVOKE_PTR_ARGS(...) RECURSE(_INVOKE_PTR_ARG_ ,##__VA_ARGS__)
#define INVOKE_PTR_ARGS(...) EVAL_16((_INVOKE_PTR_ARGS(__VA_ARGS__)))

#define UNPAREN(X) UNP(ISH X)
#define ISH(...) ISH __VA_ARGS__
#define UNP(...) UNP_(__VA_ARGS__)
#define UNP_(...) VAN ## __VA_ARGS__
#define VANISH

/** Declares a providing function that produces a test dataset.
	@param type The type that is provided
	@warning Type names with spaces in them must be parenthesized and joined with `,`,
			i.e. `struct foo` would be written as `(struct, foo)`.
			This is the only place this is necessary.
	@param name The human-readable, C-valid identifier for this dataset
*/
#define PROVIDER(type, name) \
	const char _PROVIDER_##name[] = STR(UNCOMMA(UNPAREN(type))); \
	const size_t _SIZEOF_PROVIDER_##name = sizeof(UNCOMMA(UNPAREN(type))); \
	size_t name(size_t cap, UNCOMMA(UNPAREN(type)) buf[restrict static cap]); \
	size_t CCAT(format_ , UNSEP(UNPAREN(type)) )(char *to, size_t n, const UNCOMMA(UNPAREN(type)) thing[restrict static 1]);

/** Declare a testing function. Followed by a function body using the listed arguments and returning a bool.
	@param func A human-readable, C-valid identifier for this test
	@param ... A list of every function argument, with `,` between type and name.
 */
#define TEST(func, ...) \
	const char _SIG_TEST_##func[] = JOIN(__VA_ARGS__); \
	static inline void func PAIR(__VA_ARGS__); \
	void _TEST_##func PTR_ARGS(__VA_ARGS__) \
	{ func INVOKE_PTR_ARGS(__VA_ARGS__); } \
	void func PAIR(__VA_ARGS__)

/**
	Aborts the current test run with the given error message.
	@returns Doesn't return.
*/
__attribute__((noreturn, format(printf, 1, 2)))
extern void testFailure(const char *fmt, ...);

/** Internal macro that implements `assertTrue()` */
#define assertTrueF(expr, fmt, ...) do { if( ! (expr)) testFailure("Assertion failure: Expected `%s` to be true" fmt, #expr ,##__VA_ARGS__); } while(0)
/** Asserts that an expression is truthy. Remaining arguments (optionally) give a printf-format string and corresponding parameters */
#define assertTrue(expr, ...) assertTrueF(expr, "" __VA_ARGS__)

/** Stops the current test and reports it as successful */
__attribute__((noreturn))
extern void testSuccess();

/** Considers exit() calls with any of the listed exit codes as successful.
	@note This call fails if another acceptExit() mask is already active.
		You must use `undoExpectExit` first to clear the old mask.
 */
extern void expectExit(unsigned count, const int codes[static count]);

/** Undoes a previous `acceptExit()`. */
extern void undoExpectExit();
