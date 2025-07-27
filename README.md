# CCheck
Dead simple unit testing for C with support for property-based testing.

## Setting up
To use CCheck, your project needs to provide the following:
- Subjects, shared objects that implement the interface to be tested
- Tests, subroutines that check compliance of an interface

And, if you want to use property-based tests:
- Providers, subroutines that produce typed data to be fed into tests

Usually, a C project already produces subjects.
You then need to write unit tests, and providers for any property-based tests.
These are compiled as normal shared objects, where one object may contain any number of providers or tests.

## Invoking CCheck
```
ccheck [subjects...] -- [providers/tests...]
```
Every argument names a shared object.
Unless a path explicitly contains `/`, it is searched on the standard system include path.

Providers are executed sequentially before all tests.
Individual test objects are run in parallel, and the tests inside each object are run sequentially.

### With Make
You can build ccheck with a make rule like
```make
ccheck/ccheck ccheck/integer-provider.so ccheck/interface.h:
	if [ ! -d ccheck ]; then git clone https://github.com/loglob/ccheck; fi
	make -C ccheck
```
Then invoke the tests with a rule like
```make
.PHONY: test --
test: ccheck/ccheck out/my-lib.so -- ccheck/integer-prodiver.so $(MY_PROVIDERS) $(MY_TESTS)
	./$^
```

## Writing Tests
All tests must include `interface.h`, which provides the `TEST()` macro.
A basic unit test is then written like this:
```c
TEST(myTest)
{
	assertTrue(2+2 == 4);
}
```
For property-based testing, you simply add arguments like this:
```c
TEST(myTest, uint32_t, x, uint32_t, y, struct Foo, z)
{ 
	assertTrue(x+y > x);
	int result = someMethod(z, x, y);
	assertTrue(result == 0, " got %d", result);

	if(checkForProblem(z))
		testFailure("Unexpected error, code=%d", z.error);
}
```
This wraps a function called `myTest`, accepting all the given arguments.
Fitting arguments for the function are generated using the available providers.

To indicate failure, use the `testFailure()` function, which accepts a printf-style formatted string with parameters describing the reason the test case was rejected.
For convenience, `assertTrue()` is an alias for `testFailure()` that ensures its argument is true and generates corresponding error messages.

The test run is considered successful when the function returns without calling `testFailure()`.

Calls to `exit()` and `assert()` failures in test code are also caught and considered failures.
The exit syscall itself cannot be caught so it *may* cause false positives in very specific situations.

## Writing Providers
Providers must include `interface.h`, which provides the `PROVIDER()` macro.
This macro is used to create the interface for a provider:
```c
PROVIDER((struct, Foo), provideFoo)
```
> [!IMPORTANT]
> For this macro, multi-word type names (such as `struct Foo`) must be given as parenthesized, comma-separated lists

This invocation creates two function stubs like
```c
/** Expected to fill `data` with up to `cap` entries of test data
	@param data An array to write the test values to
	@return A number `<= cap` giving the number of elements written to `data` on success.
	@return Alternatively, a number larger than `cap` if this provider would like to
		provide more than `cap` elements.
		The function will then be called again with larger capacity.
 */
size_t provideFoo(size_t cap, struct Foo data[restrict static cap]);
/** Expected to format a single value for user output.
	@param to A string buffer to write to
	@param n The capacity of `to`, including the NUL terminator (exactly like `snprintf`)
	@param data A pointer to the value to print
	@return The number of characters written, or that would have been written if there
		was not enough space in `to` (exactly like `snprintf`)
 */
size_t format_struct_Foo(char *to, size_t n, const struct Foo data[restrict static 1]);
```
that you need to define.
Note that multiple providers for the same type share a single `format_*` function.

### Builtin Integer Provider
This repo also contains a provider for integer types.
It provides the types `uint*_t` and `int*_t` from `inttypes.h`, excluding `uint8_t` and `int8_t`.
> [!NOTE]
> This provider uses most 3/4 of available bits, so additions should not overflow, however multiplications might
