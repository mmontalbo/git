#include "git-compat-util.h"
#include "odb/odb.h"
#include "pack/packfile.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct packed_git p;

	load_idx("fuzz-input", GIT_SHA1_RAWSZ, (void *)data, size, &p);

	return 0;
}
