#include "../src/JsonValue.h"
#include "../src/Utilities.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {
bool Require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        return false;
    }
    return true;
}

bool ValidateFixture(const std::filesystem::path& path) {
    std::wstring error;
    JsonValue root;
    if (!ParseJson(LoadUtf8File(path), root, error)) {
        std::wcerr << L"FAIL: parse " << path.wstring() << L": " << error << L"\n";
        return false;
    }
    const JsonValue* todos = root.get(L"todos");
    if (!Require(root.isObject(), L"root must be object") ||
        !Require(todos && todos->isArray(), L"todos must be array") ||
        !Require(!todos->arrayValue.empty(), L"todos must not be empty")) {
        return false;
    }

    const JsonValue& first = todos->arrayValue.front();
    return Require(first.isObject(), L"todo item must be object") &&
        Require(first.get(L"title") && first.get(L"title")->isString(), L"title must be string") &&
        Require(first.get(L"groupName") && first.get(L"groupName")->isString(), L"groupName must be string") &&
        Require(first.get(L"tagName") && first.get(L"tagName")->isString(), L"tagName must be string");
}
}

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
    const std::filesystem::path fixtures = root / L"tests" / L"fixtures";
    bool ok = true;
    ok = ValidateFixture(fixtures / L"todo-backup-v1.json") && ok;
    ok = ValidateFixture(fixtures / L"todo-backup-v2.json") && ok;
    ok = ValidateFixture(fixtures / L"todo-backup-apple-simple.json") && ok;
    if (!ok) {
        return 1;
    }
    std::wcout << L"Todo backup JSON compatibility fixtures passed.\n";
    return 0;
}

