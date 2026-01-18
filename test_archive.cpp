// Temporary test file for VirtualPath and VirtualFileSystem
// This file can be removed after testing

#include "VirtualPath.h"
#include "VirtualFileSystem.h"
#include <Windows.h>

void TestVirtualPath()
{
    OutputDebugStringW(L"[VirtualPath Test] Starting tests...\n");

    // Test 1: Parse regular file path
    auto vp1 = VirtualPath::Parse(L"D:\\textures\\image.dds");
    if (vp1 && !vp1->IsInArchive())
    {
        OutputDebugStringW(L"[VirtualPath Test] ✅ Regular path parsed correctly\n");
    }

    // Test 2: Parse archive path
    auto vp2 = VirtualPath::Parse(L"D:\\textures\\pack.zip\\folder\\image.dds");
    if (vp2 && vp2->IsInArchive())
    {
        std::wstring msg = L"[VirtualPath Test] ✅ Archive path parsed: " + 
                          vp2->GetDisplayPath() + L"\n";
        OutputDebugStringW(msg.c_str());
    }

    // Test 3: List directory
    VirtualPath testDir = VirtualPath::FromFilesystem(L"D:\\textures");
    auto entries = VirtualFileSystem::ListDirectory(testDir);
    
    std::wstring msg = L"[VirtualPath Test] Found " + 
                      std::to_wstring(entries.size()) + L" entries\n";
    OutputDebugStringW(msg.c_str());

    // Test 4: Filter images
    auto imageEntries = VirtualFileSystem::FilterImageEntries(entries);
    msg = L"[VirtualPath Test] Found " + 
          std::to_wstring(imageEntries.size()) + L" image entries\n";
    OutputDebugStringW(msg.c_str());

    OutputDebugStringW(L"[VirtualPath Test] Tests complete\n");
}
