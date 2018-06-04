#include "HyperLogLog.h"
#include "GeneralUtil.h"

HyperLogLog *newHyperLogLog(int m, int size) {
	HyperLogLog *b = malloc(sizeof(HyperLogLog));
	//srand(time(NULL));
	b->HLLSize = size;
	b->m = m;
	printk("m = %d\n", m);
	int size_ = sizeof(bool*) * m;
	printk("size_ = %d\n", size_);
	b->HLL = malloc(size_);
	if(!(b->HLL))printk("HLL!\n");
	else printk("1111");
	int i = 0;
	for (i = 0; i < m; i++) {
		//printf("i");
		b->HLL[i] = (bool *)malloc((32) * sizeof(bool));
		int j = 0;
		for (j = 0; j < size; j++) {
			b->HLL[i][j] = false;
		}
	}
	printf("\n");
	b->maxRegisterValue = (int)((1 << size) - 1);
	//b->alpha = getAlpha(m);
	// printf("%s\n", "This is a HyperLogLog");
	return b;
}

void encodeHyperLogLog(const HyperLogLog *b) {
	int r = rand();
	int k = r % b->m;
	//unsigned hash_val = (unsigned) hash(distribution(generator));
	unsigned hash_val = uIntHash(r);
	int leadingZeros = numberOfLeadingZeros(hash_val) + 1; // % hyperLogLog_size + hyperLogLog_size) % hyperLogLog_size;
	leadingZeros = min(leadingZeros, b->maxRegisterValue);
	if (getBitsetValue(b->HLL[k]) < leadingZeros) {
		setBitsetValue(b, k, leadingZeros);
	}
	// printf("%s\n", "This is a HyperLogLog");
}

void encodeHyperLogLogEID(const HyperLogLog *b, int elementID) {
	unsigned hash_val = uIntHash((unsigned)elementID);
	int leadingZeros = numberOfLeadingZeros(hash_val) + 1; // % hyperLogLog_size + hyperLogLog_size) % hyperLogLog_size;
	int k = (hash_val % b->m + b->m) % b->m;
	leadingZeros = min(leadingZeros, b->maxRegisterValue);
	if (getBitsetValue(b->HLL[k]) < leadingZeros) {
		setBitsetValue(b, k, leadingZeros);
	}
}

void encodeHyperLogLogSegment(const HyperLogLog *b, int flowID, int *s, int w) {
	int ms = b->m / w;
	int r = rand();
	int j = r % ms;			// (GeneralUtil::intHash(flowID) % ms + ms) % ms;
	int k = (intHash(flowID ^ s[j]) % w + w) % w;
	int i = j * w + k;
	unsigned hash_val = uIntHash(r);
	int leadingZeros = numberOfLeadingZeros(hash_val) + 1;
	leadingZeros = min(leadingZeros, b->maxRegisterValue);
	if (getBitsetValue(b->HLL[i]) < leadingZeros) {
		setBitsetValue(b, i, leadingZeros);
	}
}

void encodeHyperLogLogSegmentEID(const HyperLogLog *b, int flowID, int elementID, int *s, int w) {
	int m = b->m / w;
	int j = (intHash(elementID) % m + m) % m;
	int k = (intHash(flowID ^ s[j]) % w + w) % w;
	int i = j * w + k;
	unsigned hash_val = uIntHash(elementID);						// (unsigned)
	int leadingZeros = numberOfLeadingZeros(hash_val) + 1;
	leadingZeros = min(leadingZeros, b->maxRegisterValue);
	if (getBitsetValue(b->HLL[i]) < leadingZeros) {
		setBitsetValue(b, i, leadingZeros);
	}
}

// double getAlpha(int m)
// {
// 	double a;
// 	if (m == 16) {
// 		a = 0.673;
// 	}
// 	else if (m == 32) {
// 		a = 0.697;
// 	}
// 	else if (m == 64) {
// 		a = 0.709;
// 	}
// 	else {
// 		a = 0.7213 / (1 + 1.079 / m);
// 	}
// 	return a;
// }

int getBitsetValue(bool *b)
{
	int result = 0;
	int i = 0;
	for (i = 0; i < 5; i++) {
		if (b[i]) {
			result = result + (1 << i);
		}
	}
	return result;
}

void setBitsetValue(const HyperLogLog *b, int index, int value)
{
	int i = 0;
	while (value != 0 && i < b->HLLSize) {
		if ((value & 1) != 0) {
			b->HLL[index][i] = 1;
		}
		else {
			b->HLL[index][i] = 0;
		}
		value = value >> 1;
		i++;
	}
}