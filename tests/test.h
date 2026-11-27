// What the tests share: a check that counts, and storage that lives in memory only.
#pragma once
#include "settings.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

inline int checks = 0, failed = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Memory : settings::Storage {
    std::map<std::wstring, unsigned long> numbers;
    std::map<std::wstring, std::wstring> texts;
    int writes = 0;
    bool ReadNumber(const wchar_t* n, unsigned long& v) override {
        auto it = numbers.find(n);
        if (it == numbers.end()) return false;
        v = it->second; return true;
    }
    bool ReadText(const wchar_t* n, std::wstring& v) override {
        auto it = texts.find(n);
        if (it == texts.end()) return false;
        v = it->second; return true;
    }
    void WriteNumber(const wchar_t* n, unsigned long v) override { numbers[n] = v; writes++; }
    void WriteText(const wchar_t* n, const std::wstring& v) override { texts[n] = v; writes++; }
    void Erase(const wchar_t* n) override { numbers.erase(n); texts.erase(n); writes++; }
};
