#define _GNU_SOURCE

#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <link.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include "interface.h"

#define RED_BOLD(msg) "\x1B[31;1m" msg "\x1B[0m"
#define RED(msg) "\x1B[31m" msg "\x1B[0m"
#define YELLOW(msg) "\x1B[33m" msg "\x1B[0m"

/** Conjugates an irregular noun so that is matches the number n.
	@param n The number of items to conjugate for. Evaluated twice.
	@param x The singular form
	@param xs The plural form
	@returns inputs to a printf format like "%d %s"
 */
#define CONJUGATE3(n, x, xs) (n) , ((n) != 1 ? (xs) : (x))

/** Conjugates a noun so that it matches the number n.
	@warning Assumes a regular noun so it only adds "s"
	@returns inputs to a printf format like "%d %s"
*/
#define CONJUGATE(n, x) CONJUGATE3(n, x, x "s")

/** A set of typed data to be fed into tests */
struct Provider
{
	/** Name of the dynamic object providing this dataset */
	const char *dlName;
	/** Human readable name for this data-set.
		Determined by the second argument to the PROVIDER() macro
	 */
	const char *name;
	/** Number of items in this dataset */
	size_t count;
	/** Actual dataset */
	const void *data;
	/** Formatting function */
	format_f format;
};

/** An collection of data sets for test parameters. Forms a single-linked list. */
struct ProviderBucket
{
	/** The type name of the data, as given to the PROVIDER() macro */
	const char *type;
	/** The size of each individual element, determined automatically via sizeof().
		Sanity-checked between different providers.
	 */
	size_t elementSize;
	/** Every data set available for this type */
	struct Provider *providers;
	/** The length of providers */
	size_t count;
	/** The next bucket for a different type. */
	struct ProviderBucket *next;
};

/** Information on a dynamically linked object supplied as CLI argument */
struct DL
{
	/** The handle returned by dlopen() */
	void *handle;
	/** The start of the mapped memory at ELF offset 0 */
	const char *elfOffset;
	/** The number of entries in symbols */
	size_t symbolCount;
	/** The symbol table */
	const ElfW(Sym) *symbols;
	/** The string table */
	const char *strings;

	/** The path used as CLI argument */
	const char *name;
	/** The thread executing this object's tests */
	pthread_t runner;
	/** Whether `runner` describes a thread running in parallel or not. */
	bool parallel;
	/** Whether this object contained one or more providers. */
	bool provider;

	/** The total number of times test functions from this object were called */
	size_t variants;
	/** The number of individual tests that succeeded
		@note This number is tests, not test function calls / variants.
	*/
	size_t succeeded;
	/** The number of individual tests that failed */
	size_t failed;
};

struct ProviderBucket *providerRoot = NULL;
size_t dlCount = 0;
struct DL *dls;

/** When a provider does not give a number of data points, use this instead. */
const size_t FALLBACK_VARIANT_COUNT = 50;

/** Maximum length of message on test failure. */
#define TEST_MESSAGE_SIZE 200

/** Information on the currently running test */
__thread struct {
	/** Whether `failTarget` is currently in a valid state */
	bool jumpReady;
	/** a longjmp() target to continue at on test failure */
	jmp_buf failTarget;
	/** Stores a custom message to display in addition to the failed test's invocation */
	char message[TEST_MESSAGE_SIZE];
} runningTest = {};


void testFailure(const char *fmt, ...)
{
	va_list ls;
	va_start(ls, fmt);
	int w = vsnprintf(runningTest.message, TEST_MESSAGE_SIZE, fmt, ls);
	va_end(ls);

	if(w == TEST_MESSAGE_SIZE - 1)
	{
		for (size_t i = TEST_MESSAGE_SIZE - 4; i < TEST_MESSAGE_SIZE - 1; ++i)
			runningTest.message[i] = '.';
	}

	longjmp(runningTest.failTarget, 1);
}

// These functions overwrite stdlib symbols.
// This works only because we build with `-rdynamic` to force these symbols into `dlopen()`d objects

/** Catch stdlib exit() in test code, but not exit syscalls directly */
void exit(int status)
{	
	if(! runningTest.jumpReady)
	{
		fprintf(stderr, RED_BOLD("Got an exit(%d) from an unexpected context, aborting run!\n"), status);
		// _exit uses a syscall directly to avoid this hook
		_exit(EXIT_FAILURE);
	}

	testFailure("Test code attempted to call exit(%d)", status);
}

/** Catch assertion fails in test code */
void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *func)
{
	if(! runningTest.jumpReady)
	{
		fprintf(stderr, RED_BOLD("Got an assertion failure from an unexpected context in %s() at %s:%u\n"), func, file, line);
		// _exit uses a syscall directly to avoid the exit hook
		_exit(EXIT_FAILURE);
	}

	testFailure("Test code failed assertion in %s() at %s:%u: Expected `%s` to be true", func, file, line, assertion);
}

/** Locates a provider for the given type name */
struct ProviderBucket *findProvider(const char *type)
{
	for(struct ProviderBucket *pb = providerRoot; pb; pb = pb->next)
	{
		if(strcmp(pb->type, type) == 0)
			return pb;
	}

	return NULL;
}

/** Enumerates all vectors { v ∈ ℕⁿ. 0 ≤ vᵢ < ms[i] }
	@returns false When all values have been visited
  */
bool nextCombination(size_t n, const size_t ms[static n], size_t result[static n])
{
	for (size_t i = 0;; ++i)
	{
		if(i >= n)
			return false;

		// modular increment with carry
		if(++result[i] >= ms[i])
		{
			result[i] = 0;
			continue;
		}

		return true;
	}
}

/**
	@param bucket The bucket to grab the test data from
	@param providerIndex which provider to use from that bucket
	@param dataPosition which item to use from that provider
	@returns A pointer to the test data at the provided indices
*/
const void *locateArg(struct ProviderBucket *bucket, size_t providerIndex, size_t dataPosition)
{
	return bucket->providers[providerIndex].data  +  bucket->elementSize * dataPosition;
}

/** Invoked when a SIGSEGV signal is caught. */
void handleSignal(int signo)
{
	if(signo != SIGSEGV)
		fprintf(stderr, "Warning: handleSignal() called for non-SIGSEGV signal %u!\n", signo);
	if(! runningTest.jumpReady)
	{
		fprintf(stderr, RED_BOLD("Got a segfault from an unexpected context, aborting run!\n"));
		// _exit uses a syscall directly to avoid our exit() hook
		_exit(EXIT_FAILURE);
	}

	runningTest.jumpReady = false;
	snprintf(runningTest.message, sizeof(runningTest.message), "Caught a SIGSEGV segmentation violation");
	longjmp(runningTest.failTarget, 1);
}

/**
	@param func The dynamically located test function
	@param arity The number of function arguments `func` takes
	@param typeCount The number of unique types in the arguments of func
	@param argTypes [0 ; typeCount) -> type names
	@param argTypeIndices [0 ; arity) -> [0 ; typeCount)
	@param argNames [0 ; arity) -> argument names
 */
void runSingleTest(struct DL *dl, const char *testName, void (*const func)(), unsigned int arity, unsigned int typeCount,
	const char *restrict const argTypes[restrict static typeCount], const int argTypeIndices [restrict static arity], const char *restrict const argNames[restrict static arity])
{
	/** The buckets corresponding to the argument types */
	struct ProviderBucket *typeBuckets[typeCount];
	/** i |-> typeBuckets[i].count */
	size_t bucketSizes[typeCount];

	// locate provider buckets
	for(size_t i = 0; i < typeCount; ++i)
	{
		struct ProviderBucket *pb = findProvider(argTypes[i]);

		if(pb == NULL)
		{
			fprintf(stderr, RED_BOLD("Couldn't run test") " %s::%s: No providers registered for type '%s'.\n", dl->name, testName, argTypes[i]);
			++dl->failed;
			return;
		}

		typeBuckets[i] = pb;
		bucketSizes[i] = pb->count;
	}

	/** i |-> The current selection of provider from typeBuckets[i] */
	size_t curProviders[typeCount];
	/** i |-> The number of elements provided by curProviders[argTypeIndices[i]] */
	size_t curDataCounts[arity];
	/** i |-> The current selection of test data index for argument i in [0; curDataCounts[i])  */
	size_t curDataIndices[arity];

	memset(curProviders, 0, sizeof(curProviders));

	if(setjmp(runningTest.failTarget))
	{
		runningTest.jumpReady = false;
		++dl->failed;

		const size_t cap = 2048;
		char *buffer = malloc(cap + 1);

		if(buffer == NULL)
		{
			printf(RED_BOLD("Failed test") " %s::%s and malloc() failed when trying to allocate an error message\n", dl->name, testName);
			return;
		}

		size_t w = 0;
		w += snprintf(buffer + w, cap - w, RED_BOLD("Failed test") " %s::%s(", dl->name, testName);

		for (unsigned int i = 0; i < arity; ++i)
		{
			size_t ti = argTypeIndices[i];
			struct ProviderBucket *pb = typeBuckets[ti];
			struct Provider p = pb->providers[curProviders[ti]];

			w += snprintf(buffer + w, cap - w, "%s %s = ", i ? "," : "", argNames[i]);
			w += p.format(buffer + w, cap - w, locateArg(pb, curProviders[ti], curDataIndices[i]));
			w += snprintf(buffer + w, cap - w, " (%s::%s #%zu)", p.dlName, p.name, curDataIndices[i]);
		}

		w += snprintf(buffer + w, cap - w, " ): %s\n", runningTest.message);

		fputs(buffer, stdout);
		free(buffer);

		return;
	}

	do
	{
		// cache counts for nextCombination()
		for (size_t i = 0; i < arity; ++i)
		{
			size_t ti = argTypeIndices[i];
			curDataCounts[i] = typeBuckets[ti]->providers[curProviders[ti]].count;
		}

		memset(curDataIndices, 0, sizeof(curDataIndices));

		do
		{
			#define arg(i) locateArg(typeBuckets[argTypeIndices[i]], curProviders[argTypeIndices[i]], curDataIndices[i])
			++dl->variants;

			runningTest.jumpReady = true;

			switch(arity)
			{
				case MAX_ARITY:
					func( arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7) );
				break;

				case 7:
					func( arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6) );
				break;

				case 6:
					func( arg(0), arg(1), arg(2), arg(3), arg(4), arg(5) );
				break;

				case 5:
					func( arg(0), arg(1), arg(2), arg(3), arg(4) );
				break;

				case 4:
					func( arg(0), arg(1), arg(2), arg(3) );
				break;

				case 3:
					func( arg(0), arg(1), arg(2) );
				break;

				case 2:
					func( arg(0), arg(1) );
				break;

				case 1:
					func( arg(0) );
				break;

				case 0:
					func();
				break;

				default:
					__builtin_unreachable();
			}

			runningTest.jumpReady = false;

			#undef arg
		} while(nextCombination(arity, curDataCounts, curDataIndices));
	} while(nextCombination(typeCount, bucketSizes, curProviders));

	++dl->succeeded;
}

/** Runs every test in a dynamic object
	@param dl A non-null loaded object
 */
void runTests(struct DL *dl)
{
	if(dl->symbolCount == 0 || dl->symbols == NULL || dl->strings == NULL)
		return;

	/** [0 ; typeCount) */
	const char *argTypes[MAX_ARITY];
	/** [0 ; arity) -> [0 ; typeCount) */
	int argTypeIndices[MAX_ARITY];
	/** [0 ; arity) */
	const char *argNames[MAX_ARITY];


	for(size_t i = 1; i < dl->symbolCount; ++i)
	{
		ElfW(Sym) s = dl->symbols[i];
		const char *name = dl->strings + s.st_name;
		const char *testName = name + 10;

		if(strncmp(name, "_SIG_TEST_", 10) != 0)
			continue;

		bool (*func)() = dlsym(dl->handle, name + 4);

		if(func == NULL) {
			fprintf(stderr, RED_BOLD("Couldn't run test") " %s::%s: Missing testing function: dlsym(): %s\n", dl->name, name, dlerror()); // dlerror in pthread, cringe
			++dl->failed;
			goto skip_symbol;
		}

		unsigned int arity = 0;
		// <= arity
		unsigned int typeCount = 0;

		// populate argTypes, argNames, and argTypeIndices
		for(const char *cur = dl->elfOffset + s.st_value; *cur; ++arity)
		{
			if(arity >= MAX_ARITY)
			{
				fprintf(stderr, RED_BOLD("Couldn't run test") " %s::%s: Arity is greater than the maximum of %u.\n", dl->name, name + 10, MAX_ARITY);
				++dl->failed;
				goto skip_symbol;
			}

			const char *type = cur;
			cur += strlen(cur) + 1;
			argNames[arity] = cur;
			cur += strlen(cur) + 1;

			unsigned int typeIdx = typeCount;

			for(unsigned int i = 0; i < typeCount; ++i)
			{
				if(strcmp(argTypes[i], type) == 0)
				{
					typeIdx = i;
					break;
				}
			}

			argTypeIndices[arity] = typeIdx;

			if(typeIdx == typeCount)
				argTypes[typeCount++] = type;
		}

		runSingleTest(dl, testName, dlsym(dl->handle, name + 4), arity, typeCount, argTypes, argTypeIndices, argNames);

		skip_symbol:;
	}

	if(dl->variants)
	{
		printf("\x1B[%umModule %s: Ran %zu %s with %zu %s, %zu %s\x1B[0m\n",
			dl->failed ? 31 : 92, dl->name, CONJUGATE(dl->failed + dl->succeeded, "test"), CONJUGATE(dl->variants, "variant"), CONJUGATE(dl->failed, "failure"));
	}
	else if(! dl->provider)
		printf(YELLOW("Module %s provided no data and contained no tests\n"), dl->name);
}

/** Wrapper for `runTests()` as a pthread entry point
	@param _dl A non-null `struct DL` pointer
	@returns NULL
 */
void *_runTests(void *_dl)
{
	runTests(_dl);
	return NULL;
}

/** Loads a provider from a dynamic object
	@param sizeof_provider_name The name of the _SIZE_PROVIDER_* symbol
	@param size The value of the const named by `sizeof_provider_name`
	@returns true if loading succeeded
	@returns false and prints an error message otherwise
 */
bool loadOneProvider(struct DL *dl, const char *sizeof_provider_name, size_t size)
{
	const char *name = sizeof_provider_name + 17; // cur off _SIZEOF_PROVIDER_

	#define chkDlsym(type, varname, symbol) type varname = dlsym(dl->handle, symbol); \
		if(varname == NULL) { fprintf(stderr, "Failed to load provider %s::%s: Missing symbol '%s': %s\n", dl->name, name, symbol, dlerror()); return false; }

	chkDlsym(const char*, type, sizeof_provider_name + 7) // cut off _SIZEOF
	chkDlsym(provider_f, prov, name) // cut off _SIZEOF

	char nameBuf[200] = "format_";
	strncat(nameBuf, type, sizeof(nameBuf) - 1);

	// tr '_' ' '
	for(char *b = nameBuf; *b; ++b)
	{
		if(*b == ' ')
			*b = '_';
	}

	chkDlsym(format_f, fmt, nameBuf);
	void *buf = NULL;

	if(setjmp(runningTest.failTarget))
	{
		fprintf(stderr, RED("Failed to run provider %s::%s: %s\n"), dl->name, name, runningTest.message);

		if(buf)
			free(buf);

		return false;
	}

	runningTest.jumpReady = true;
	size_t n = prov(0, NULL);
	runningTest.jumpReady = false;

	if(n == 0)
		n = FALLBACK_VARIANT_COUNT;

	#define checkMalloc(ptr, ...) if(ptr == NULL) { fprintf(stderr, "Failed to load provider %s::%s: Malloc failure\n", dl->name, name); __VA_ARGS__; return false; }
	buf = malloc(n * size);
	checkMalloc(buf)

	runningTest.jumpReady = true;
	size_t m = prov(n, buf);
	runningTest.jumpReady = false;

	if(m > n)
	{
		fprintf(stderr, "Failed to load provider %s::%s: Unexpected size return\n", dl->name, name);
		free(buf);
		return false;
	}

	if(m < n)
	{
		void *r = realloc(buf, m * size);

		if(r)
			buf = r;

		n = m;
	}

	struct ProviderBucket *b = findProvider(type);
	bool newB = false;

	if(!b)
	{
		newB = true;
		b = malloc(sizeof(struct ProviderBucket));
		checkMalloc(b, free(buf))

		b->count = 0;
		b->elementSize = size;
		b->next = NULL;
		b->providers = NULL;
		b->type = type;
	}
	else if(b->elementSize != size)
	{
		fprintf(stderr, "Failed to load provider '%s': Size mismatch between other %s providers\n", name, type);
		free(buf);
		return false;
	}

	struct Provider *np = realloc(b->providers, sizeof(struct Provider) * (b->count + 1));
	checkMalloc(np, free(buf); if(newB) free(b); );

	np[b->count++] = (struct Provider) {
		.count = n,
		.data = buf,
		.dlName = dl->name,
		.name = name,
		.format = fmt
	};
	b->providers = np;

	if(newB)
	{
		b->next = providerRoot;
		providerRoot = b;
	}

	return true;
}

/** Searches a dynamic objet for providers
	@returns The number of providers found */
size_t loadProviders(struct DL *dl)
{
	size_t count = 0;

	for(size_t i = 1; i < dl->symbolCount; ++i)
	{
		ElfW(Sym) s = dl->symbols[i];
		const char *name = dl->strings + s.st_name;

		if(strncmp(name, "_SIZEOF_PROVIDER_", 17) != 0)
			continue;

		count += loadOneProvider(dl, name, *(const size_t*)(dl->elfOffset + s.st_value));
	}

	dl->provider = count > 0;

	return count;
}

/** Initializes a DL reference
	@param handle A non-null pointer returned by dlopen()
	@param name A name identifying that dynamic object
	@returns false and prints an error message on failure
*/
bool loadDL(void *handle, const char *name)
{
	bool success = true;
	struct link_map *lm;
	int e = dlinfo(handle, RTLD_DI_LINKMAP, &lm);

	if(e)
	{
		fprintf(stderr, RED_BOLD("Error loading") " '%s': dlinfo(): %s\n", name, dlerror());
		return false;
	}

	struct DL dl = {
		.handle = handle,
		.name = name,
		.elfOffset = (void*)lm->l_addr
	};

	// iterate over tags
	for(const ElfW(Dyn) *d = lm->l_ld; d->d_tag != DT_NULL; ++d)
	{
		switch(d->d_tag)
		{
			case DT_GNU_HASH:
			{
				// GNU_HASH layout described here: https://flapenguin.me/elf-dt-gnu-hash
				const uint32_t *ht = (uint32_t*)d->d_un.d_ptr;
				uint32_t nb = ht[0], symOff = ht[1], bloomSize = ht[2];

				const ElfW(Addr) *bloom = (void*)(ht + 4);

				// check if bloom filter is populated, otherwise buckets and chains are invalid & lead to segfault
				for(uint32_t i = 0; i < bloomSize; ++i)
				{
					if(bloom[i])
						goto bloom_ok;
				}

				dl.symbolCount = 1;
				break;

				bloom_ok:;
				// maximum symbol table index
				size_t maxInd = symOff;
				const uint32_t *buckets = (uint32_t*)(bloom + bloomSize);

				for(size_t i = 0; i < nb; ++i)
				{
					size_t b = buckets[i];

					if(b > maxInd)
						maxInd = b;
				}

				const uint32_t *chain = buckets + nb;

				// walk chain to the end
				while((chain[maxInd - symOff] & 1) == 0)
					++maxInd;

				dl.symbolCount = maxInd + 1;
			}
			break;

			case DT_HASH:
				dl.symbolCount = 1[ (uint32_t*)d->d_un.d_ptr ];
			break;

			case DT_SYMENT:
			{
				size_t expect = sizeof(ElfW(Sym));

				if(d->d_un.d_val != expect)
				{
					fprintf(stderr,  YELLOW("Warning loading '%s': Got symbol entry size %zu whereas %zu was expected.\n"),
						name, d->d_un.d_val, expect);
				}
			}
			break;

			case DT_SYMTAB:
				dl.symbols = (void*)d->d_un.d_ptr;
			break;

			case DT_STRTAB:
				dl.strings = (void*)d->d_un.d_ptr;
			break;
		}
	}

	if(dl.symbolCount == 0)
	{
		fprintf(stderr, RED_BOLD("Error loading") " '%s': Couldn't determine symbol table size", name);
		success = false;
	}
	if(dl.symbols == NULL)
	{
		fprintf(stderr, RED_BOLD("Error loading") " '%s': Couldn't find symbol table", name);
		success = false;
	}
	if(dl.strings == NULL)
	{
		fprintf(stderr, RED_BOLD("Error loading ") "'%s': Couldn't find strings table", name);
		success = false;
	}

	if(success)
		dls[dlCount++] = dl;

	return success;
}

int main(int argc, char **argv)
{
	bool linkerErrors = false;

	if(argc == 0)
	{
		fprintf(stderr,
			"Usage: %s [subjects...] -- [providers/testers...]\n"
			"Every argument is a shared object file.\n"
			"'subjects' are the libraries being tested. Their symbols are exposed to the following testers.\n"
			"The following objects expose providers which generate data sets, "
				"and test cases which consume those values and check the interface exposed by subjects.\n", argv[0]);
		return 1;
	}

	size_t provCount = 0;
	size_t subjectCount = 0;
	void *subjects[argc];
	struct DL _dls[argc];
	dls = _dls;
	bool gotSeparator = false;

	srand(clock());

	struct sigaction sa = {};
	sa.sa_handler = handleSignal;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SA_NODEFER);
	
	if(sigaction(SIGSEGV, &sa, NULL))
		fprintf(stderr, YELLOW("Segfaults will not be caught due to sigaction() error: %s\n"), strerror(errno));

	// load DLs and providers
	for(int i = 1; i < argc; ++i)
	{
		if(!gotSeparator && strcmp(argv[i], "--") == 0)
		{
			gotSeparator = true;
			continue;
		}

		void *handle = dlopen(argv[i], RTLD_NOW | (gotSeparator ? RTLD_LOCAL : RTLD_GLOBAL));

		if(handle == NULL)
		{
			fprintf(stderr, RED_BOLD("Error loading")" '%s': %s\n", argv[i], dlerror());
			linkerErrors = true;
			continue;
		}

		// subjects are only loaded for the side effects from dlopen()
		if(! gotSeparator)
		{
			subjects[subjectCount++] = handle;
			continue;
		}

		if(! loadDL(handle, argv[i]))
		{
			dlclose(handle);
			linkerErrors = true;
			continue;
		}

		provCount += loadProviders(&dls[dlCount - 1]);
	}

	printf("Loaded %zu %s and %zu %s.\n", CONJUGATE(subjectCount, "subject"), CONJUGATE(provCount, "provider"));

	for(size_t i = 0; i < dlCount; ++i)
	{
		int e = pthread_create( &dls[i].runner, NULL, _runTests, &dls[i] );

		if(!(dls[i].parallel = !e))
			fprintf(stderr, YELLOW("Running '%s' in series due to pthread_create() error: %s\n"), dls[i].name, strerror(e));
	}

	for(size_t i = 0; i < dlCount; ++i)
	{
		if(!dls[i].parallel)
			runTests(&dls[i]);
	}

	size_t totalSucceeded = 0, totalFailed = 0, totalVariants = 0;

	for(size_t i = 0; i < dlCount; ++i)
	{
		if(dls[i].parallel)
		{
			int e = pthread_join(dls[i].runner, NULL);

			if(e)
				fprintf(stderr, YELLOW("Error joining thread of '%s': pthread_join(): %s\n"), dls[i].name, strerror(e));
		}

		totalSucceeded += dls[i].succeeded;
		totalFailed += dls[i].failed;
		totalVariants += dls[i].variants;
	}

	printf("Summary: Ran %zu %s from %zu %s with %zu %s,\x1B[%u;1m got %zu %s\x1B[0m\n",
		CONJUGATE(totalSucceeded + totalFailed, "test"), CONJUGATE(dlCount, "module"), CONJUGATE(totalVariants, "variant"),
		totalFailed ? 31 : 92, CONJUGATE(totalFailed, "failure"));

	if(linkerErrors)
		puts(RED("There were linking errors"));

	for(size_t i = 0; i < subjectCount; ++i)
		dlclose(subjects[i]);
	for(size_t i = 0; i < dlCount; ++i)
		dlclose(dls[i].handle);
	for(struct ProviderBucket *b = providerRoot; b;)
	{
		for (size_t i = 0; i < b->count; ++i)
			free((void*) b->providers[i].data);

		struct ProviderBucket *next = b->next;
		free(b->providers);
		free(b);
		b = next;
	}

	return linkerErrors || totalFailed > 0;
}