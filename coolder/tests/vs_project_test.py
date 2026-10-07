"""Validate native VS project references and source coverage without MSBuild."""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
CONFIGS = {
    f"{c}|{p}"
    for c in ("Debug", "Release")
    for p in ("Win32", "x64", "ARM64", "ARM64EC")
}


def read_project(path):
    return ET.parse(path).getroot()


def items(root, tag):
    return {e.attrib["Include"] for e in root.findall(f"./m:ItemGroup/m:{tag}", NS)}


class VisualStudioProjectTests(unittest.TestCase):
    def test_solution_dependencies_and_configurations(self):
        solution = (ROOT / "coolder.sln").read_text(encoding="utf-8-sig")
        projects = re.findall(
            r'Project\("[^"\n]+"\) = "([^"]+)", "([^"]+)", "([^"]+)"', solution
        )
        self.assertEqual(len(projects), 9)
        by_path = {}
        for name, relative, guid in projects:
            path = (ROOT / relative.replace("\\", "/")).resolve()
            project = read_project(path)
            self.assertEqual(
                project.findtext(
                    "./m:PropertyGroup/m:ProjectGuid", namespaces=NS
                ).upper(),
                guid.upper(),
                name,
            )
            self.assertTrue(CONFIGS <= items(project, "ProjectConfiguration"), name)
            for config in CONFIGS:
                for suffix in ("ActiveCfg", "Build.0"):
                    self.assertIn(f"{guid}.{config}.{suffix} = {config}", solution)
            by_path[path] = guid
        app = read_project(ROOT / "coolder.vcxproj")
        refs = app.findall("./m:ItemGroup/m:ProjectReference", NS)
        self.assertEqual(len(refs), 8)
        for ref in refs:
            path = (ROOT / ref.attrib["Include"].replace("\\", "/")).resolve()
            self.assertEqual(ref.findtext("m:Project", namespaces=NS), by_path[path])
            links = ref.findtext("m:LinkLibraryDependencies", namespaces=NS)
            self.assertEqual(
                links, "false" if "sandbox-helper" in path.name else "true"
            )

    def test_source_coverage_and_filters(self):
        for filename, expected in (
            (
                "coolder.vcxproj",
                {
                    ROOT / "main.cpp",
                    *ROOT.glob("server/*.cpp"),
                    *ROOT.glob("action/**/*.cpp"),
                },
            ),
            (
                "coolder-sandbox-helper.vcxproj",
                {ROOT / "sandbox/sandbox_helper_main.cpp"},
            ),
        ):
            project = read_project(ROOT / filename)
            filters = read_project(ROOT / (filename + ".filters"))
            actual = {ROOT / p.replace("\\", "/") for p in items(project, "ClCompile")}
            self.assertEqual(actual, expected)
            for tag in ("ClCompile", "ClInclude", "None"):
                self.assertEqual(items(project, tag), items(filters, tag))
                for relative in items(project, tag):
                    self.assertTrue(
                        (ROOT / relative.replace("\\", "/")).is_file(), relative
                    )
        helper = read_project(ROOT / "coolder-sandbox-helper.vcxproj")
        self.assertEqual(
            helper.findtext("./m:PropertyGroup/m:TargetName", namespaces=NS),
            "webcool-sandbox-helper",
        )

    def test_resource_packaging(self):
        project = read_project(ROOT / "coolder.vcxproj")
        bundle = project.find("./m:Target[@Name='BundleCoolderFrontend']", NS)
        self.assertIn(
            "bundle_frontend.cmake", bundle.find("m:Exec", NS).attrib["Command"]
        )
        copy = project.find("./m:Target[@Name='CopyCoolderRuntime']", NS)
        self.assertIsNotNone(copy.find("./m:ItemGroup/m:CoolderFrontend", NS))
        self.assertIsNotNone(copy.find("./m:MSBuild[@Targets='GetTargetPath']", NS))
        props = read_project(ROOT / "coolder.windows.props")
        defines = props.findtext(
            "./m:ItemDefinitionGroup/m:ClCompile/m:PreprocessorDefinitions",
            namespaces=NS,
        )
        self.assertIn('COOLDER_HTML_DIR="html"', defines)


if __name__ == "__main__":
    unittest.main()
