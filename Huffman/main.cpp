#include <vector>
#include "huffman.h"

void verify_block_api(Huffman &huff, std::vector<Huffman::Value> &input);


int main(int argc, char *argv[])
{
	int i;
	bool compress, decompress, iswide, verify, blocktest;
	char *base_filename;
	Huffman huff;
	std::vector<Huffman::Value> input;

	if (argc < 2) {
		printf("Usage: huffman <file> -b -c -d -w -v\n");
		exit(0);
	}

	compress = false;
	decompress = false;
	iswide = false;
	verify = false;
	blocktest = false;
	base_filename = nullptr;
	for (i = 1; i < argc; ++i) {
		if (argv[i][0] == '-') {
			if (argv[i][1] == 'b') {
				blocktest = true;
				continue;
			}
			if (argv[i][1] == 'c') {
				compress = true;
				continue;
			}
			if (argv[i][1] == 'd') {
				decompress = true;
				continue;
			}
			if (argv[i][1] == 'v') {
				verify = true;
				compress = true;
				continue;
			}
			if (argv[i][1] == 'w') {
				iswide = true;				/* Input file has uint16_t values. */
				continue;
			}
			printf("Bad option char\n");
			exit(1);
		}
		else {
			/* Must be a filename. */
			base_filename = argv[i];
		}
	}

	if (base_filename == nullptr) {
		printf("No base filename given.\n");
		exit(1);
	}
	if (blocktest) {
		/* Test the block compress and decompress functions. */
		huff.read_input(base_filename, iswide, input);
		huff.process_input(input);
		verify_block_api(huff, input);
		return(0);
	}
	if (compress && decompress) {
		printf("Conflicting options\n");
		exit(1);
	}
	if (compress == false && decompress == false) {
		/* No action option. Read the input file, generate and print the codetable. */
		huff.read_input(base_filename, iswide, input);
		huff.process_input(input);
		huff.print_codetable();
		return(0);
	}
	if (compress) {
		/* Read input file, generate a compressed .huff output file and a .hdr header file. */
		huff.compress(base_filename, iswide, verify);
		return(0);
	}
	if (decompress) {
		std::vector<uint16_t> uncompressed;

		/* Read a .huff, .hdr compressed file set, write uncompressed file. */
		huff.decompress(base_filename, uncompressed);
		return(0);
	}
	return(0);
}


void verify_block_api(Huffman &huff, std::vector<Huffman::Value> &input)
{
	uint32_t start_index, next_start_index;
	std::vector<uint32_t> block;
	std::vector<Huffcode> codelengths;
	std::vector<Huffman::Lengthtable> lengthtable;
	std::vector<Huffman::Value> uncompr;

	huff.get_codelengths(codelengths);
	qsort(&codelengths[0], codelengths.size(), sizeof(Huffcode), compare_huffcode_entries);
	generate_codes(codelengths);
	build_length_table(codelengths, lengthtable);
	block.resize(128 / sizeof(uint32_t), 0);
	for (start_index = 0; start_index < input.size(); ) {
		next_start_index = huff.write_compressed_block(input, start_index, block);
		uncompress_block(block, next_start_index - start_index, uncompr, lengthtable, codelengths);
		for (size_t i = 0; i < uncompr.size(); ++i) {
			if (uncompr[i] != input[start_index + i]) {
				printf("Verify error at input[i + start_index]: expected %d, got %d\n", input[start_index + i], uncompr[i]);
			}
		}
		start_index = next_start_index;
	}
}


