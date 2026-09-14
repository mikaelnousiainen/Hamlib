#!/usr/bin/env python3
"""Convert cppcheck XML (version 2) output to SARIF 2.1.0 for upload to
GitHub code scanning.

Usage: cppcheck-xml2sarif.py <cppcheck.xml> <out.sarif>
"""

import json
import sys
import xml.etree.ElementTree as ET

# Findings that are about the analysis run itself, not the code.
SKIP_IDS = {
    "missingInclude", "missingIncludeSystem", "unknownMacro",
    "toomanyconfigs", "checkersReport", "unmatchedSuppression",
    "syntaxError", "internalError", "normalCheckLevelMaxBranches",
}

LEVELS = {
    "error": "error",
    "warning": "warning",
    "performance": "note",
    "portability": "note",
    "style": "note",
    "information": "note",
}


def norm_path(path):
    path = path.replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    return path


def main():
    xml_file, out_file = sys.argv[1], sys.argv[2]
    root = ET.parse(xml_file).getroot()

    results = []
    rules = {}
    for err in root.iter("error"):
        rule_id = err.get("id", "unknown")
        severity = err.get("severity", "style")
        if rule_id in SKIP_IDS or severity == "debug":
            continue
        locations = [loc for loc in err.findall("location")
                     if loc.get("file")]
        if not locations:
            continue
        if rule_id not in rules:
            rule = {
                "id": rule_id,
                "shortDescription": {"text": err.get("msg", rule_id)},
                "properties": {"tags": ["cppcheck", severity]},
            }
            cwe = err.get("cwe")
            if cwe:
                rule["properties"]["tags"].append("external/cwe/cwe-" + cwe)
            rules[rule_id] = rule
        message = err.get("verbose") or err.get("msg") or rule_id
        results.append({
            "ruleId": rule_id,
            "level": LEVELS.get(severity, "note"),
            "message": {"text": message},
            "locations": [{
                "physicalLocation": {
                    "artifactLocation": {
                        "uri": norm_path(loc.get("file")),
                    },
                    "region": {
                        "startLine": max(1, int(loc.get("line", "1") or 1)),
                    },
                },
            } for loc in locations[:1]],
        })

    cppcheck = root.find("cppcheck")
    version = cppcheck.get("version") if cppcheck is not None else "unknown"
    sarif = {
        "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/"
                   "master/Schemata/sarif-schema-2.1.0.json",
        "version": "2.1.0",
        "runs": [{
            "tool": {
                "driver": {
                    "name": "cppcheck",
                    "version": version,
                    "informationUri": "https://cppcheck.sourceforge.io/",
                    "rules": sorted(rules.values(), key=lambda r: r["id"]),
                },
            },
            "results": results,
        }],
    }

    with open(out_file, "w", encoding="utf-8") as fh:
        json.dump(sarif, fh, indent=1)
    print(f"cppcheck-xml2sarif: wrote {len(results)} result(s) to {out_file}")


if __name__ == "__main__":
    main()
