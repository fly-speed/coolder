#include "stdafx.h"
#include "../validation/ctest_io_template.h"
#include "project_scaffold.h"

#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"
#include "supported_languages.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace webcool
{
namespace ai
{
namespace
{

struct scaffold_file_t {
	std::string path;
	std::string content;
	bool executable;

	scaffold_file_t(const std::string &file_path,
			const std::string &file_content,
			bool file_executable = false)
		: path(file_path)
		, content(file_content)
		, executable(file_executable)
	{
	}
};

std::string join_relative(const std::string &parent, const std::string &child)
{
	return parent.empty() ? child : parent + "/" + child;
}

// Build systems impose stricter identifier rules than workspace paths. Keep
// the user's directory name while deriving a portable ASCII target name.
std::string portable_project_name(const std::string &project_path)
{
	const size_t slash = project_path.rfind('/');
	const std::string leaf = slash == std::string::npos ?
					 project_path :
					 project_path.substr(slash + 1);
	std::string value;
	for (size_t i = 0; i < leaf.size(); ++i) {
		const unsigned char ch = static_cast<unsigned char>(leaf[i]);
		value += std::isalnum(ch) ?
				 static_cast<char>(std::tolower(ch)) :
				 '_';
	}
	if (value.empty())
		value = "webcool_project";
	if (std::isdigit(static_cast<unsigned char>(value[0])))
		value.insert(0, "app_");
	return value;
}

void add_script_files(
	const std::string &platform, const std::string &shell_build,
	const std::string &shell_run, const std::string &shell_clean,
	const std::string &batch_build, const std::string &batch_run,
	const std::string &batch_clean, std::vector<scaffold_file_t> &files)
{
	if (platform == "linux" || platform == "macos" ||
	    platform == "cross-platform") {
		files.push_back(scaffold_file_t("build.sh", shell_build, true));
		files.push_back(scaffold_file_t("run.sh", shell_run, true));
		files.push_back(scaffold_file_t("clean.sh", shell_clean, true));
	}
	if (platform == "windows" || platform == "cross-platform") {
		files.push_back(scaffold_file_t("build.bat", batch_build));
		files.push_back(scaffold_file_t("run.bat", batch_run));
		files.push_back(scaffold_file_t("clean.bat", batch_clean));
	}
}

void build_cpp_or_c_files(const std::string &language,
			  const std::string &platform,
			  const std::string &project_name,
			  std::vector<scaffold_file_t> &files)
{
	const bool cpp = language == "cpp";
	std::ostringstream cmake;
	cmake << "cmake_minimum_required(VERSION 3.16)\n"
	      << "project(" << project_name << " LANGUAGES "
	      << (cpp ? "CXX" : "C") << ")\n\n";
	if (cpp) {
		files.push_back(scaffold_file_t("tests/webcool_io_test.cmake",
						ctest_io_template()));
		files.push_back(scaffold_file_t("tests/webcool_io_tests.cmake",
						ctest_registration_template()));
		cmake << "set(CMAKE_CXX_STANDARD 17)\n"
		      << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
		      << "set(CMAKE_CXX_EXTENSIONS OFF)\n\n";
		files.push_back(scaffold_file_t(
			"src/stdafx.h",
			"#ifndef WEBCOOL_PROJECT_STDAFX_H\n#define WEBCOOL_PROJECT_STDAFX_H\n\n"
			"// Common C++17 standard-library headers. Keep project headers self-contained.\n"
			"#include <algorithm>\n#include <array>\n#include <atomic>\n"
			"#include <cassert>\n#include <cctype>\n#include <cerrno>\n#include <chrono>\n"
			"#include <cmath>\n#include <condition_variable>\n#include <cstddef>\n"
			"#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n"
			"#include <deque>\n#include <exception>\n#include <filesystem>\n"
			"#include <fstream>\n#include <functional>\n#include <future>\n"
			"#include <iomanip>\n#include <ios>\n#include <iostream>\n#include <istream>\n"
			"#include <iterator>\n#include <limits>\n#include <list>\n#include <map>\n"
			"#include <memory>\n#include <mutex>\n#include <numeric>\n#include <optional>\n"
			"#include <ostream>\n#include <queue>\n#include <random>\n#include <set>\n"
			"#include <sstream>\n#include <stack>\n#include <stdexcept>\n#include <string>\n"
			"#include <string_view>\n#include <system_error>\n#include <thread>\n"
			"#include <tuple>\n#include <type_traits>\n#include <unordered_map>\n"
			"#include <unordered_set>\n#include <utility>\n#include <variant>\n#include <vector>\n\n"
			"#endif  // WEBCOOL_PROJECT_STDAFX_H\n"));
	} else {
		cmake << "set(CMAKE_C_STANDARD 11)\n"
		      << "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
		      << "set(CMAKE_C_EXTENSIONS OFF)\n\n";
	}
	cmake << "add_executable(" << project_name << " src/main."
	      << (cpp ? "cpp" : "c") << ")\n";
	if (cpp)
		cmake << "target_include_directories(" << project_name
		      << " PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/src\")\n";
	files.push_back(scaffold_file_t("CMakeLists.txt", cmake.str()));
	files.push_back(scaffold_file_t(
		cpp ? "src/main.cpp" : "src/main.c",
		cpp ? "#include \"stdafx.h\"\n\nint main() {\n\tstd::cout << \"Hello from WebCool!\" << std::endl;\n\treturn 0;\n}\n" :
		      "#include <stdio.h>\n\nint main(void) {\n\tputs(\"Hello from WebCool!\");\n\treturn 0;\n}\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\ncmake -S . -B build\ncmake --build build\n",
		"#!/bin/sh\nset -eu\nexec ./build/" + project_name +
			" \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build\n",
		"@echo off\r\ncmake -S . -B build\r\nif errorlevel 1 exit /b %errorlevel%\r\ncmake --build build\r\n",
		"@echo off\r\nbuild\\" + project_name + ".exe %*\r\n",
		"@echo off\r\nif exist build rmdir /s /q build\r\n", files);
	files.push_back(scaffold_file_t(".gitignore", "build/\n"));
}

void build_javascript_files(const std::string &platform,
			    const std::string &project_name,
			    std::vector<scaffold_file_t> &files)
{
	std::ostringstream package;
	package << "{\n  \"name\": \"" << project_name
		<< "\",\n  \"version\": \"0.1.0\",\n  \"private\": true,\n"
		<< "  \"scripts\": { \"start\": \"node src/main.js\", "
		<< "\"test\": \"node --test\" }\n}\n";
	files.push_back(scaffold_file_t("package.json", package.str()));
	files.push_back(scaffold_file_t(
		"src/main.js",
		"'use strict';\n\nfunction greeting() {\n  return 'Hello from WebCool!';\n}\n\n"
		"if (require.main === module) console.log(greeting());\n\n"
		"module.exports = { greeting };\n"));
	files.push_back(scaffold_file_t(
		"tests/main.test.js",
		"'use strict';\n\nconst test = require('node:test');\n"
		"const assert = require('node:assert/strict');\n"
		"const { greeting } = require('../src/main');\n\n"
		"test('greeting is stable', () => {\n"
		"  assert.equal(greeting(), 'Hello from WebCool!');\n});\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\nnode --check src/main.js\nnode --test\n",
		"#!/bin/sh\nset -eu\nexec node src/main.js \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build coverage .nyc_output\n",
		"@echo off\r\nnode --check src\\main.js\r\nif errorlevel 1 exit /b %errorlevel%\r\nnode --test\r\n",
		"@echo off\r\nnode src\\main.js %*\r\n",
		"@echo off\r\nif exist build rmdir /s /q build\r\nif exist coverage rmdir /s /q coverage\r\nif exist .nyc_output rmdir /s /q .nyc_output\r\n",
		files);
	files.push_back(scaffold_file_t(".gitignore", "node_modules/\n"));
}

void build_python_files(const std::string &platform,
			const std::string &project_name,
			std::vector<scaffold_file_t> &files)
{
	std::ostringstream project;
	project << "[project]\nname = \"" << project_name
		<< "\"\nversion = \"0.1.0\"\nrequires-python = \">=3.9\"\n"
		<< "dependencies = []\n";
	files.push_back(scaffold_file_t("pyproject.toml", project.str()));
	files.push_back(scaffold_file_t(
		"src/main.py",
		"def greeting():\n    return \"Hello from WebCool!\"\n\n\n"
		"def main():\n    print(greeting())\n\n\n"
		"if __name__ == \"__main__\":\n    main()\n"));
	files.push_back(scaffold_file_t(
		"tests/test_main.py",
		"import unittest\n\nfrom src.main import greeting\n\n\n"
		"class GreetingTest(unittest.TestCase):\n"
		"    def test_greeting(self):\n"
		"        self.assertEqual(greeting(), \"Hello from WebCool!\")\n\n\n"
		"if __name__ == \"__main__\":\n    unittest.main()\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\npython3 -m py_compile src/main.py\npython3 -m unittest discover -s tests\n",
		"#!/bin/sh\nset -eu\nexec python3 src/main.py \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build dist src/__pycache__ tests/__pycache__\n",
		"@echo off\r\npython -m py_compile src\\main.py\r\nif errorlevel 1 exit /b %errorlevel%\r\npython -m unittest discover -s tests\r\n",
		"@echo off\r\npython src\\main.py %*\r\n",
		"@echo off\r\nif exist build rmdir /s /q build\r\nif exist dist rmdir /s /q dist\r\nif exist src\\__pycache__ rmdir /s /q src\\__pycache__\r\nif exist tests\\__pycache__ rmdir /s /q tests\\__pycache__\r\n",
		files);
	files.push_back(scaffold_file_t(".gitignore",
					"__pycache__/\n*.py[cod]\n.venv/\n"));
}

void build_java_files(const std::string &platform,
		      std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t(
		"src/Main.java",
		"public final class Main {\n    private Main() {}\n\n"
		"    public static String greeting() {\n"
		"        return \"Hello from WebCool!\";\n    }\n\n"
		"    public static void main(String[] args) {\n"
		"        System.out.println(greeting());\n    }\n}\n"));
	files.push_back(scaffold_file_t(
		"tests/MainTest.java",
		"public final class MainTest {\n    private MainTest() {}\n\n"
		"    public static void main(String[] args) {\n"
		"        if (!\"Hello from WebCool!\".equals(Main.greeting())) {\n"
		"            throw new AssertionError(\"unexpected greeting\");\n"
		"        }\n    }\n}\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\nmkdir -p build\njavac -d build src/Main.java tests/MainTest.java\njava -cp build MainTest\n",
		"#!/bin/sh\nset -eu\nexec java -cp build Main \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build\n",
		"@echo off\r\nif not exist build mkdir build\r\njavac -d build src\\Main.java tests\\MainTest.java\r\nif errorlevel 1 exit /b %errorlevel%\r\njava -cp build MainTest\r\n",
		"@echo off\r\njava -cp build Main %*\r\n",
		"@echo off\r\nif exist build rmdir /s /q build\r\n", files);
	files.push_back(scaffold_file_t(".gitignore", "build/\n*.class\n"));
}

void build_go_files(const std::string &platform,
		    const std::string &project_name,
		    std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t("go.mod", "module example.com/" +
							  project_name +
							  "\n\ngo 1.21\n"));
	files.push_back(scaffold_file_t(
		"src/main.go",
		"package main\n\nimport \"fmt\"\n\nfunc greeting() string {\n"
		"\treturn \"Hello from WebCool!\"\n}\n\n"
		"func main() {\n\tfmt.Println(greeting())\n}\n"));
	files.push_back(scaffold_file_t(
		"src/main_test.go",
		"package main\n\nimport \"testing\"\n\nfunc TestGreeting(t *testing.T) {\n"
		"\tif greeting() != \"Hello from WebCool!\" {\n"
		"\t\tt.Fatal(\"unexpected greeting\")\n\t}\n}\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\nmkdir -p build\ngo build -o build/" +
			project_name + " ./src\n",
		"#!/bin/sh\nset -eu\nexec ./build/" + project_name +
			" \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build\n",
		"@echo off\r\nif not exist build mkdir build\r\ngo build -o build\\" +
			project_name + ".exe ./src\r\n",
		"@echo off\r\nbuild\\" + project_name + ".exe %*\r\n",
		"@echo off\r\nif exist build rmdir /s /q build\r\n", files);
	files.push_back(
		scaffold_file_t(".gitignore", "build/\n.webcool-go-app\n"));
}

void build_rust_files(const std::string &platform,
		      const std::string &project_name,
		      std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t(
		"Cargo.toml",
		"[package]\nname = \"" + project_name +
			"\"\nversion = \"0.1.0\"\nedition = \"2021\"\n\n"
			"[dependencies]\n"));
	files.push_back(scaffold_file_t(
		"src/main.rs",
		"fn greeting() -> &'static str {\n    \"Hello from WebCool!\"\n}\n\n"
		"fn main() {\n    println!(\"{}\", greeting());\n}\n\n"
		"#[cfg(test)]\nmod tests {\n    use super::greeting;\n\n"
		"    #[test]\n    fn greeting_is_stable() {\n"
		"        assert_eq!(greeting(), \"Hello from WebCool!\");\n    }\n}\n"));
	add_script_files(platform, "#!/bin/sh\nset -eu\ncargo build\n",
			 "#!/bin/sh\nset -eu\nexec cargo run -- \"$@\"\n",
			 "#!/bin/sh\nset -eu\ncargo clean\n",
			 "@echo off\r\ncargo build\r\n",
			 "@echo off\r\ncargo run -- %*\r\n",
			 "@echo off\r\ncargo clean\r\n", files);
	files.push_back(scaffold_file_t(".gitignore", "target/\n"));
}

void build_objective_c_files(const std::string &project_name,
			     std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t(
		"src/main.m",
		"#import <Foundation/Foundation.h>\n\nNSString *WebCoolGreeting(void) {\n"
		"    return @\"Hello from WebCool!\";\n}\n\nint main(void) {\n"
		"    @autoreleasepool {\n        NSLog(@\"%@\", WebCoolGreeting());\n"
		"    }\n    return 0;\n}\n"));
	files.push_back(scaffold_file_t(
		"tests/main_test.m",
		"#import <Foundation/Foundation.h>\n\n"
		"NSString *WebCoolGreeting(void);\n\nint main(void) {\n"
		"    @autoreleasepool {\n"
		"        if (![WebCoolGreeting() isEqualToString:@\"Hello from WebCool!\"]) return 1;\n"
		"    }\n    return 0;\n}\n"));
	files.push_back(scaffold_file_t(
		"Makefile",
		"APP := " + project_name +
			"\n\n.PHONY: all test clean\nall: build/$(APP)\n\nbuild/$(APP): src/main.m\n"
			"\tmkdir -p build\n\txcrun --sdk macosx clang -fobjc-arc "
			"-framework Foundation src/main.m -o $@\n\ntest:\n"
			"\tmkdir -p build\n\txcrun --sdk macosx clang -fobjc-arc "
			"-Dmain=WebCoolApplicationMain -c src/main.m -o build/main.o\n"
			"\txcrun --sdk macosx clang -fobjc-arc -framework Foundation "
			"tests/main_test.m build/main.o -o build/tests\n"
			"\t./build/tests\n\nclean:\n\trm -rf build\n"));
	files.push_back(scaffold_file_t("build.sh",
					"#!/bin/sh\nset -eu\nmake\n", true));
	files.push_back(scaffold_file_t("run.sh",
					"#!/bin/sh\nset -eu\nexec ./build/" +
						project_name + " \"$@\"\n",
					true));
	files.push_back(scaffold_file_t(
		"clean.sh", "#!/bin/sh\nset -eu\nmake clean\n", true));
	files.push_back(scaffold_file_t(".gitignore", "build/\n"));
}

void build_swift_files(const std::string &platform,
		       const std::string &project_name,
		       std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t(
		"Package.swift",
		"// swift-tools-version: 5.9\nimport PackageDescription\n\n"
		"let package = Package(\n    name: \"" +
			project_name +
			"\",\n"
			"    targets: [\n        .executableTarget(name: \"" +
			project_name +
			"\", path: \"src\"),\n"
			"        .testTarget(name: \"" +
			project_name + "Tests\", dependencies: [\"" +
			project_name + "\"], path: \"Tests\")\n    ]\n)\n"));
	files.push_back(scaffold_file_t(
		"src/main.swift",
		"import Foundation\n\npublic func greeting() -> String {\n"
		"    \"Hello from WebCool!\"\n}\n\nprint(greeting())\n"));
	files.push_back(scaffold_file_t(
		"Tests/GreetingTests.swift",
		"import XCTest\n@testable import " + project_name +
			"\n\n"
			"final class GreetingTests: XCTestCase {\n"
			"    func testGreeting() {\n"
			"        XCTAssertEqual(greeting(), \"Hello from WebCool!\")\n"
			"    }\n}\n"));
	add_script_files(platform, "#!/bin/sh\nset -eu\nswift build\n",
			 "#!/bin/sh\nset -eu\nexec swift run " + project_name +
				 " \"$@\"\n",
			 "#!/bin/sh\nset -eu\nswift package clean\n",
			 "@echo off\r\nswift build\r\n",
			 "@echo off\r\nswift run " + project_name + " %*\r\n",
			 "@echo off\r\nswift package clean\r\n", files);
	files.push_back(scaffold_file_t(".gitignore", ".build/\n.swiftpm/\n"));
}

void build_csharp_files(const std::string &platform,
			const std::string &project_name,
			std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t(
		project_name + ".csproj",
		"<Project Sdk=\"Microsoft.NET.Sdk\">\n  <PropertyGroup>\n"
		"    <OutputType>Exe</OutputType>\n    <TargetFramework>net8.0</TargetFramework>\n"
		"    <ImplicitUsings>enable</ImplicitUsings>\n"
		"    <Nullable>enable</Nullable>\n  </PropertyGroup>\n"
		"  <ItemGroup><Compile Remove=\"tests/**/*.cs\" /></ItemGroup>\n</Project>\n"));
	files.push_back(scaffold_file_t(
		"src/Program.cs",
		"Console.WriteLine(Greeting.Text);\n\n"
		"public static class Greeting\n{\n"
		"    public const string Text = \"Hello from WebCool!\";\n}\n"));
	files.push_back(scaffold_file_t(
		"tests/Tests.csproj",
		"<Project Sdk=\"Microsoft.NET.Sdk\">\n  <PropertyGroup>\n"
		"    <OutputType>Exe</OutputType>\n"
		"    <TargetFramework>net8.0</TargetFramework>\n"
		"  </PropertyGroup>\n  <ItemGroup>\n"
		"    <ProjectReference Include=\"../" +
			project_name +
			".csproj\" />\n  </ItemGroup>\n</Project>\n"));
	files.push_back(scaffold_file_t(
		"tests/Program.cs",
		"using System;\n\n"
		"if (Greeting.Text != \"Hello from WebCool!\")\n"
		"    throw new Exception(\"unexpected greeting\");\n"));
	add_script_files(
		platform, "#!/bin/sh\nset -eu\ndotnet build -o build\n",
		"#!/bin/sh\nset -eu\nexec dotnet run --project " +
			project_name + ".csproj -- \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build bin obj tests/bin tests/obj\n",
		"@echo off\r\ndotnet build -o build\r\n",
		"@echo off\r\ndotnet run --project " + project_name +
			".csproj -- %*\r\n",
		"@echo off\r\nfor %%d in (build bin obj tests\\bin tests\\obj) do if exist %%d rmdir /s /q %%d\r\n",
		files);
	files.push_back(scaffold_file_t(".gitignore", "build/\nobj/\n"));
}

void build_kotlin_files(const std::string &platform,
			std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t(
		"src/Main.kt",
		"fun greeting(): String = \"Hello from WebCool!\"\n\n"
		"fun main() {\n    println(greeting())\n}\n"));
	files.push_back(scaffold_file_t(
		"tests/MainTest.kt",
		"object MainTest {\n    @JvmStatic\n"
		"    fun main(args: Array<String>) {\n"
		"        check(greeting() == \"Hello from WebCool!\")\n"
		"    }\n}\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\nmkdir -p build\nkotlinc src/Main.kt -include-runtime -d build/app.jar\n",
		"#!/bin/sh\nset -eu\nexec java -jar build/app.jar \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build\n",
		"@echo off\r\nif not exist build mkdir build\r\nkotlinc src\\Main.kt -include-runtime -d build\\app.jar\r\n",
		"@echo off\r\njava -jar build\\app.jar %*\r\n",
		"@echo off\r\nif exist build rmdir /s /q build\r\n", files);
	files.push_back(scaffold_file_t(
		".gitignore", "build/\n*.class\n.webcool-kotlin-*.jar\n"));
}

void build_php_files(const std::string &platform,
		     const std::string &project_name,
		     std::vector<scaffold_file_t> &files)
{
	files.push_back(scaffold_file_t(
		"composer.json", "{\n  \"name\": \"webcool/" + project_name +
					 "\",\n  \"type\": \"project\",\n"
					 "  \"require\": {}\n}\n"));
	files.push_back(scaffold_file_t(
		"src/main.php",
		"<?php\n\ndeclare(strict_types=1);\n\nfunction greeting(): string\n{\n"
		"    return 'Hello from WebCool!';\n}\n\necho greeting(), PHP_EOL;\n"));
	files.push_back(scaffold_file_t(
		"tests/main_test.php",
		"<?php\n\ndeclare(strict_types=1);\n\nob_start();\n"
		"require __DIR__ . '/../src/main.php';\nob_end_clean();\n\n"
		"if (greeting() !== 'Hello from WebCool!') {\n"
		"    throw new RuntimeException('unexpected greeting');\n}\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\nphp -l src/main.php\nphp tests/main_test.php\n",
		"#!/bin/sh\nset -eu\nexec php src/main.php \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -rf build .phpunit.cache\n",
		"@echo off\r\nphp -l src\\main.php\r\nif errorlevel 1 exit /b %errorlevel%\r\nphp tests\\main_test.php\r\n",
		"@echo off\r\nphp src\\main.php %*\r\n",
		"@echo off\r\nif exist build rmdir /s /q build\r\nif exist .phpunit.cache rmdir /s /q .phpunit.cache\r\n",
		files);
	files.push_back(scaffold_file_t(".gitignore", "vendor/\n"));
}

void build_d_files(const std::string &platform, const std::string &project_name,
		   std::vector<scaffold_file_t> &files)
{
	std::string package_name = project_name;
	std::replace(package_name.begin(), package_name.end(), '_', '-');
	files.push_back(scaffold_file_t(
		"dub.json",
		"{\n  \"name\": \"" + package_name +
			"\",\n  \"targetType\": \"executable\",\n"
			"  \"sourcePaths\": [\"src\"],\n  \"mainSourceFile\": \"main.d\",\n"
			"  \"dependencies\": {}\n}\n"));
	files.push_back(scaffold_file_t(
		"src/main.d",
		"import std.stdio;\n\nstring greeting()\n{\n"
		"    return \"Hello from WebCool!\";\n}\n\nvoid main()\n{\n"
		"    writeln(greeting());\n}\n"));
	files.push_back(scaffold_file_t(
		"tests/main.d",
		"module main_test;\n\nunittest\n{\n"
		"    assert(\"Hello from WebCool!\" == \"Hello from WebCool!\");\n}\n\n"
		"void main() {}\n"));
	add_script_files(
		platform,
		"#!/bin/sh\nset -eu\ndmd -of=build-app src/main.d\ndmd -unittest -run tests/main.d\n",
		"#!/bin/sh\nset -eu\nexec ./build-app \"$@\"\n",
		"#!/bin/sh\nset -eu\nrm -f build-app build-app.exe *.o *.obj\n",
		"@echo off\r\ndmd -of=build-app.exe src\\main.d\r\nif errorlevel 1 exit /b %errorlevel%\r\ndmd -unittest -run tests\\main.d\r\n",
		"@echo off\r\nbuild-app.exe %*\r\n",
		"@echo off\r\nif exist build-app del /q build-app\r\nif exist build-app.exe del /q build-app.exe\r\ndel /q *.obj *.o 2>nul\r\n",
		files);
	files.push_back(scaffold_file_t(
		".gitignore",
		"build-app\nbuild-app.exe\n.webcool-d-app\n.webcool-d-app.exe\n*.obj\n*.o\n"));
}

std::vector<scaffold_file_t> scaffold_files(const std::string &language,
					    const std::string &platform,
					    const std::string &project_name)
{
	std::vector<scaffold_file_t> files;
	if (language == "cpp" || language == "c") {
		build_cpp_or_c_files(language, platform, project_name, files);
	} else if (language == "javascript") {
		build_javascript_files(platform, project_name, files);
	} else if (language == "python") {
		build_python_files(platform, project_name, files);
	} else if (language == "java") {
		build_java_files(platform, files);
	} else if (language == "go") {
		build_go_files(platform, project_name, files);
	} else if (language == "rust") {
		build_rust_files(platform, project_name, files);
	} else if (language == "objective-c") {
		build_objective_c_files(project_name, files);
	} else if (language == "swift") {
		build_swift_files(platform, project_name, files);
	} else if (language == "csharp") {
		build_csharp_files(platform, project_name, files);
	} else if (language == "kotlin") {
		build_kotlin_files(platform, files);
	} else if (language == "php") {
		build_php_files(platform, project_name, files);
	} else if (language == "d") {
		build_d_files(platform, project_name, files);
	}
	std::ostringstream readme;
	readme << "# " << project_name << "\n\nGenerated by WebCool.\n\n"
	       << "- Language: " << language << "\n- Platform: " << platform
	       << "\n";
	files.push_back(scaffold_file_t("README.md", readme.str()));
	return files;
}

void rollback_scaffold(agent_workspace_t &workspace,
		       const std::string &project_path,
		       const std::vector<scaffold_file_t> &files,
		       size_t created_files,
		       const std::vector<std::string> &created_directories,
		       bool project_created,
		       const std::vector<std::string> &created_parents)
{
	std::string rollback_err;
	while (created_files > 0) {
		--created_files;
		const scaffold_file_t &file = files[created_files];
		if (!workspace.delete_text_if_unchanged(
			    join_relative(project_path, file.path),
			    agent_workspace_t::content_sha256(file.content),
			    rollback_err)) {
			ai_log_error("project.scaffold", "rollback-file",
				     rollback_err);
		}
	}
	for (size_t i = created_directories.size(); i > 0; --i) {
		if (!workspace.delete_empty_directory(
			    join_relative(project_path,
					  created_directories[i - 1]),
			    rollback_err)) {
			ai_log_error("project.scaffold", "rollback-directory",
				     rollback_err);
		}
	}
	if (project_created &&
	    !workspace.delete_empty_directory(project_path, rollback_err)) {
		ai_log_error("project.scaffold", "rollback-project",
			     rollback_err);
	}
	for (size_t i = created_parents.size(); i > 0; --i) {
		if (!workspace.delete_empty_directory(created_parents[i - 1],
						      rollback_err)) {
			ai_log_error("project.scaffold",
				     "rollback-project-parent", rollback_err);
		}
	}
}

bool prepare_scaffold_parent(agent_workspace_t &workspace,
			     const std::string &parent,
			     std::vector<std::string> &created,
			     std::string &err)
{
	created.clear();
	std::string current;
	size_t offset = 0;
	while (offset < parent.size()) {
		const size_t slash = parent.find('/', offset);
		const std::string component = parent.substr(
			offset, slash == std::string::npos ? std::string::npos :
							     slash - offset);
		const std::string next = join_relative(current, component);
		std::vector<workspace_entry_t> entries;
		if (!workspace.list(current, entries, err))
			break;
		bool exists = false;
		bool directory = false;
		for (size_t i = 0; i < entries.size(); ++i) {
			if (entries[i].path != next)
				continue;
			exists = true;
			directory = entries[i].directory;
			break;
		}
		if (exists && !directory) {
			err = "workspace path already exists";
			break;
		}
		if (!exists) {
			if (!workspace.create_directory_if_absent(next, err))
				break;
			created.push_back(next);
		}
		current = next;
		if (slash == std::string::npos)
			return true;
		offset = slash + 1;
	}
	std::string rollback_err;
	for (size_t i = created.size(); i > 0; --i) {
		if (!workspace.delete_empty_directory(created[i - 1],
						      rollback_err)) {
			ai_log_error("project.scaffold",
				     "rollback-project-parent", rollback_err);
		}
	}
	created.clear();
	return parent.empty();
}

std::vector<std::string>
scaffold_directories(const std::vector<scaffold_file_t> &files)
{
	// Derive every parent directory from the generated file list. This keeps the
	// transaction generic when a language adds Tests/, nested packages or other
	// conventional source roots, and lets rollback remove them in reverse order.
	std::vector<std::string> directories;
	for (size_t i = 0; i < files.size(); ++i) {
		size_t slash = files[i].path.find('/');
		while (slash != std::string::npos) {
			const std::string directory =
				files[i].path.substr(0, slash);
			if (std::find(directories.begin(), directories.end(),
				      directory) == directories.end())
				directories.push_back(directory);
			slash = files[i].path.find('/', slash + 1);
		}
	}
	std::sort(directories.begin(), directories.end(),
		  [](const std::string &left, const std::string &right) {
			  const size_t left_depth =
				  std::count(left.begin(), left.end(), '/');
			  const size_t right_depth =
				  std::count(right.begin(), right.end(), '/');
			  return left_depth == right_depth ?
					 left < right :
					 left_depth < right_depth;
		  });
	return directories;
}

} // namespace

bool create_project_scaffold(agent_workspace_t &workspace,
			     const std::string &project_path,
			     const std::string &language,
			     const std::string &platform,
			     project_scaffold_result_t &result,
			     std::string &err)
{
	result = project_scaffold_result_t();
	if (!supported_project_language(language)) {
		err = "unsupported project language";
		return ai_error("project.scaffold", "validate-language", err);
	}
	if (!supported_project_platform(platform)) {
		err = "unsupported project platform";
		return ai_error("project.scaffold", "validate-platform", err);
	}
	if (!project_language_supports_platform(language, platform)) {
		err = "selected project language does not support the selected platform";
		return ai_error("project.scaffold",
				"validate-language-platform", err);
	}
	std::string normalized;
	if (!agent_workspace_t::normalize_path(project_path, normalized, false,
					       err)) {
		return ai_error("project.scaffold", "normalize-project", err);
	}
	const std::vector<scaffold_file_t> files = scaffold_files(
		language, platform, portable_project_name(normalized));

	// The directory tree '+' button creates its child immediately so users can
	// select that exact location. Accept such a directory only while it is still
	// empty; an existing file or non-empty directory remains a hard conflict.
	const size_t slash = normalized.rfind('/');
	const std::string parent =
		slash == std::string::npos ? "" : normalized.substr(0, slash);
	std::vector<std::string> created_parents;
	if (!prepare_scaffold_parent(workspace, parent, created_parents, err)) {
		return ai_error("project.scaffold", "prepare-project-parent",
				err);
	}
	std::vector<workspace_entry_t> siblings;
	if (!workspace.list(parent, siblings, err)) {
		rollback_scaffold(workspace, normalized, files, 0,
				  std::vector<std::string>(), false,
				  created_parents);
		return ai_error("project.scaffold", "inspect-project-parent",
				err);
	}
	bool project_exists = false;
	bool project_is_directory = false;
	for (size_t i = 0; i < siblings.size(); ++i) {
		if (siblings[i].path == normalized) {
			project_exists = true;
			project_is_directory = siblings[i].directory;
			break;
		}
	}
	bool project_created = false;
	if (project_exists) {
		if (!project_is_directory) {
			err = "workspace path already exists";
			rollback_scaffold(workspace, normalized, files, 0,
					  std::vector<std::string>(), false,
					  created_parents);
			return ai_error("project.scaffold",
					"inspect-project-type", err);
		}
		std::vector<workspace_entry_t> children;
		if (!workspace.list(normalized, children, err)) {
			rollback_scaffold(workspace, normalized, files, 0,
					  std::vector<std::string>(), false,
					  created_parents);
			return ai_error("project.scaffold",
					"inspect-project-directory", err);
		}
		if (!children.empty()) {
			err = "workspace project directory is not empty";
			rollback_scaffold(workspace, normalized, files, 0,
					  std::vector<std::string>(), false,
					  created_parents);
			return ai_error("project.scaffold",
					"reject-nonempty-project", err);
		}
		result.reused_empty_directory = true;
	} else {
		if (!workspace.create_directory_if_absent(normalized, err)) {
			rollback_scaffold(workspace, normalized, files, 0,
					  std::vector<std::string>(), false,
					  created_parents);
			return false;
		}
		project_created = true;
	}

	const std::vector<std::string> directories =
		scaffold_directories(files);
	std::vector<std::string> created_directories;
	for (size_t i = 0; i < directories.size(); ++i) {
		if (!workspace.create_directory_if_absent(
			    join_relative(normalized, directories[i]), err)) {
			rollback_scaffold(workspace, normalized, files, 0,
					  created_directories, project_created,
					  created_parents);
			return ai_error("project.scaffold", "create-directory",
					err);
		}
		created_directories.push_back(directories[i]);
	}
	size_t created = 0;
	for (; created < files.size(); ++created) {
		if (!workspace.create_text_if_absent(
			    join_relative(normalized, files[created].path),
			    files[created].content, err)) {
			rollback_scaffold(workspace, normalized, files, created,
					  created_directories, project_created,
					  created_parents);
			return ai_error("project.scaffold", "create-file", err);
		}
		result.files.push_back(
			join_relative(normalized, files[created].path));
		if (files[created].executable &&
		    !workspace.set_text_executable(result.files.back(), err)) {
			rollback_scaffold(workspace, normalized, files,
					  created + 1, created_directories,
					  project_created, created_parents);
			result.files.clear();
			return ai_error("project.scaffold",
					"make-file-executable", err);
		}
	}
	result.language = language;
	result.platform = platform;
	return true;
}

} // namespace ai
} // namespace webcool
