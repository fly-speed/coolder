#pragma once
#include <fstream>
#include <sstream>
#include <string>
namespace libai
{
namespace ai
{
namespace record_json
{
// Translate validated versioned record tables to named JSON fields.
std::string encode(const std::string &text, const std::string &path);
// Decode JSON records into the version-specific parser's input contract.
std::string decode(const std::string &text);
// Read JSON or a legacy record file; legacy validation remains with its store.
class input : public std::istringstream {
public:
	// Open an existing record file without mutating its contents.
	explicit input(
	    const std::string &path, std::ios::openmode mode = std::ios::in);
	// Close the memory-backed reader.
	void close()
	{
	}
};
// Stage a JSON record file; existing stores retain their atomic install logic.
class output : public std::ostringstream {
	// Owned temporary output stream.
	std::ofstream file_;
	// Path selects the record schema for serialization.
	std::string path_;
	// Prevent repeated serialization during destruction.
	bool closed_ = false;

public:
	// Open the store-owned temporary file.
	explicit output(
	    const std::string &path, std::ios::openmode mode = std::ios::out);
	// Flush structured JSON to the temporary file and propagate write errors.
	void close();
	// Complete a pending write on normal scope exit.
	~output();
};
}
}
}
