#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <initializer_list>
#include <fstream>
#include <iostream>
#include <xaudio2.h>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <atomic>

bool sounds = true;
float volume = 1.0f;
bool keyPressSound = true;
bool keyReleaseSound = true;
bool keyboardMode = false;
std::string audioPath = "audio/turquoise";

bool copilot = false;
bool cooldown = false;

std::vector<WORD> pressed = {};

std::vector<std::string> clipboard(10, "");

bool typing = false;
std::string commandBuffer = "";

struct Sound;
struct Palette;
bool LoadSound(const std::string& filename, Sound& sound);
bool checkWFX(const WAVEFORMATEX& a, const WAVEFORMATEX& b);

IXAudio2* pXAudio2 = nullptr;
IXAudio2MasteringVoice* pMasterVoice = nullptr;

bool keyPressed[256] = {false};

Palette* pressedPalette;
Palette* releasedPalette;

struct macroEvent;

std::vector<macroEvent> events;
bool isRecordingMacro = false;
bool isChangingRepeat = false;
std::atomic<bool> isPlayingMacro = false;
std::atomic<bool> plsStop = false;
int repeatMacro = 1;

DWORD macroStartTime = 0;

bool slashPressed = false;

struct Sound {
    std::vector<BYTE> audioData;
    WAVEFORMATEX wfx;
};

struct Palette {
    Sound Backspace;
    Sound Enter;
    Sound Space;
    std::vector<Sound> Generics;

    IXAudio2SourceVoice* pBackspaceVoice = nullptr;
    IXAudio2SourceVoice* pEnterVoice = nullptr;
    IXAudio2SourceVoice* pSpaceVoice = nullptr;

    std::vector<IXAudio2SourceVoice*> pGenericPool;
    int idx = 0;

    Palette(std::string path) {
        if (!LoadSound(path + "/BACKSPACE.wav", Backspace))
            LoadSound(path + "/GENERIC_R0.wav", Backspace);
        if (!LoadSound(path + "/ENTER.wav", Enter))
            LoadSound(path + "/GENERIC_R0.wav", Enter);
        if (!LoadSound(path + "/SPACE.wav", Space))
            LoadSound(path + "/GENERIC_R0.wav", Space);

        pXAudio2->CreateSourceVoice(&pBackspaceVoice, &Backspace.wfx);
        pXAudio2->CreateSourceVoice(&pEnterVoice, &Enter.wfx);
        pXAudio2->CreateSourceVoice(&pSpaceVoice, &Space.wfx);

        for (int i = 0; true; i++) {
            Sound generic;
            std::string filename = path + "/GENERIC_R" + std::to_string(i) + ".wav";
            if (LoadSound(filename, generic)) {
                Generics.push_back(generic);
            } else {
                break;
            }
        }

        for (int i = 1; i < Generics.size(); i++) {
            if (!checkWFX(Generics[i].wfx, Generics[0].wfx)) {
                std::cerr << "Warning: GENERIC_R" << i << " and GENERIC_R0 have different WAVEFORMATEX!" << std::endl;
            }
        }

        for (int i = 0; i < 10; i++) {
            IXAudio2SourceVoice* pVoice = nullptr;
            pXAudio2->CreateSourceVoice(&pVoice, &Generics[0].wfx);
            pGenericPool.push_back(pVoice);
        }
    }

    ~Palette() {
        pBackspaceVoice->DestroyVoice();
        pEnterVoice->DestroyVoice();
        pSpaceVoice->DestroyVoice();
        for (auto voice : pGenericPool) {
            voice->DestroyVoice();
        }
    }

    void playSound(int key) {
        IXAudio2SourceVoice* pVoice = nullptr;
        Sound* sound = nullptr;
        XAUDIO2_BUFFER buffer = {0};

        switch (key) {
            case VK_BACK:
                pVoice = pBackspaceVoice;
                sound = &Backspace;
                break;
            case VK_RETURN:
                pVoice = pEnterVoice;
                sound = &Enter;
                break;
            case VK_SPACE:
                pVoice = pSpaceVoice;
                sound = &Space;
                break;
            default:
                pVoice = pGenericPool[idx];
                sound = &Generics[rand() % Generics.size()];
                idx = (idx + 1) % pGenericPool.size();
                break;
        }

        pVoice->Stop(0);
        pVoice->FlushSourceBuffers();

        buffer.AudioBytes = sound->audioData.size();
        buffer.pAudioData = sound->audioData.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        pVoice->SubmitSourceBuffer(&buffer);
        pVoice->Start(0);
    }
    
};

struct macroEvent{
    DWORD vkCode;
    UINT msg;
    DWORD delay;
};

bool checkWFX(const WAVEFORMATEX& a, const WAVEFORMATEX& b) {
    return a.wFormatTag == b.wFormatTag &&
        a.nChannels == b.nChannels &&
        a.nSamplesPerSec == b.nSamplesPerSec &&
        a.wBitsPerSample == b.wBitsPerSample;
}

bool LoadSound(const std::string& filename, Sound& sound) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;

    char chunkId[4];
    int chunkSize;

    file.seekg(12);

    while (file.read(chunkId, 4)) {
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (memcmp(chunkId, "fmt ", 4) == 0) {
            memset(&sound.wfx, 0, sizeof(WAVEFORMATEX));
            file.read(reinterpret_cast<char*>(&sound.wfx), chunkSize);
        } else if (memcmp(chunkId, "data", 4) == 0) {
            sound.audioData.resize(chunkSize);
            file.read(reinterpret_cast<char*>(sound.audioData.data()), chunkSize);
            return true;
        } else {
            file.seekg(chunkSize + (chunkSize % 2), std::ios::cur);
        }
    }
    return false;
}

void KeyPress(WORD vkCode) {
    INPUT input;

    input.type = INPUT_KEYBOARD;
    input.ki.wScan = 0;
    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;
    input.ki.wVk = vkCode;
    input.ki.dwFlags = 0;

    SendInput(1, &input, sizeof(INPUT));
}

void KeyRelease(WORD vkCode) {
    INPUT input;

    input.type = INPUT_KEYBOARD;
    input.ki.wScan = 0;
    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;
    input.ki.wVk = vkCode;
    input.ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(1, &input, sizeof(INPUT));
}

void KeyClick(WORD vkCode) {
    KeyPress(vkCode);
    KeyRelease(vkCode);
}

void KeyCombo(std::initializer_list<WORD> keys) {
    KeyRelease(VK_LWIN);
    KeyRelease(VK_LSHIFT);

    INPUT* inputs = new INPUT[keys.size() * 2]();
    
    for (int i = 0; i < keys.size(); i++) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = keys.begin()[i];

        inputs[keys.size()*2 - 1 - i].type = INPUT_KEYBOARD;
        inputs[keys.size()*2 - 1 - i].ki.wVk = keys.begin()[i];
        inputs[keys.size()*2 - 1 - i].ki.dwFlags = KEYEVENTF_KEYUP;

        // std::cout << "Prepared key: " << keys.begin()[i] << " at indices " << i << " and " << (keys.size()*2 - 1 - i) << std::endl;
    }

    SendInput(keys.size() * 2, inputs, sizeof(INPUT));

    delete[] inputs;
}

void mouseEvent(WORD button){
    INPUT input = {0};

    input.type = INPUT_MOUSE;
    input.mi.dwFlags = button;

    SendInput(1, &input, sizeof(INPUT));
}

void mouseClick(bool left){
    if (left) {
        mouseEvent(MOUSEEVENTF_LEFTDOWN);
        mouseEvent(MOUSEEVENTF_LEFTUP);
    } else {
        mouseEvent(MOUSEEVENTF_RIGHTDOWN);
        mouseEvent(MOUSEEVENTF_RIGHTUP);
    }
}

std::string getClipboard(){
    if (!OpenClipboard(NULL)) return "";

    HANDLE hData = GetClipboardData(CF_TEXT);
    if (hData == NULL) {
        CloseClipboard();
        return "";
    }

    char* pszText = static_cast<char*>(GlobalLock(hData));
    std::string text = "";
    if (pszText != NULL) {
        text = pszText;
    }

    GlobalUnlock(hData);
    CloseClipboard();

    return text;
}

void setClipboard(const std::string& text){
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();

    size_t size = text.length() + 1;
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hGlobal) {
        CloseClipboard();
        return;
    }

    memcpy(GlobalLock(hGlobal), text.c_str(), size);
    GlobalUnlock(hGlobal);

    SetClipboardData(CF_TEXT, hGlobal);
    CloseClipboard();
}

bool handleC(){
    int index = -1;
    for (WORD key : pressed){
        if (key >= '0' && key <= '9'){
            index = key - '0';
            break;
        }
    }
    if (index == -1) return false;

    KeyCombo({VK_LCONTROL, 'C'});
    Sleep(30);
    clipboard[index] = getClipboard();

    Sleep(30);
    setClipboard("");

    return true;
}

bool handleV(){
    int index = -1;
    for (WORD key : pressed){
        if (key >= '0' && key <= '9'){
            index = key - '0';
            break;
        }
    }
    if (index == -1) return false;

    std::string temp = getClipboard();

    setClipboard(clipboard[index]);
    Sleep(30);
    KeyCombo({VK_LCONTROL, 'V'});
    
    Sleep(30);
    setClipboard(temp);

    return true;
}

void recordMacro(DWORD key, UINT msg){
    if (key == VK_F23) {
        if (!events.empty())
            events.pop_back();
        if (!events.empty())
            events.pop_back();

        isRecordingMacro = false;

        KeyRelease(VK_LWIN);
        KeyRelease(VK_LSHIFT);
        copilot = false;
        pressed.clear();
        cooldown = true;

        return;
    }

    DWORD currentTime = GetTickCount();
    DWORD delay = currentTime - macroStartTime;

    macroEvent event = {key, msg, delay};
    events.push_back(event);
}

void playMacro(){
    plsStop = false;

    for (int i = 0; i < repeatMacro; i++){
        macroStartTime = GetTickCount();

        for (int j = 1; j < events.size(); j++){
            macroEvent event = events[j];

            DWORD currentTime = GetTickCount();
            DWORD elapsed = currentTime - macroStartTime;
            DWORD waitTime = (event.delay > elapsed) ? (event.delay - elapsed) : 0;
            Sleep(waitTime);

            if (plsStop) {
                isPlayingMacro = false;
                events.clear();
                return;
            }

            if (event.msg == WM_KEYDOWN){
                KeyPress(event.vkCode);
            } else if (event.msg == WM_KEYUP){
                KeyRelease(event.vkCode);
            }
        }
    }

    isPlayingMacro = false;
}

bool handleM(){
    if (isRecordingMacro || isPlayingMacro || isChangingRepeat)
        return false;

    int type = -1;
    for (WORD key : pressed){
        if (key >= '1' && key <= '3'){
            type = key - '0';
            break;
        }
    }

    if (type == 1){
        isRecordingMacro = true;
        isPlayingMacro = false;
        macroStartTime = GetTickCount();
        events.clear();
        return true;
    } else if (type == 2){
        isRecordingMacro = false;
        isPlayingMacro = true;
        macroStartTime = GetTickCount();
        std::thread(playMacro).detach();
        return true;
    } else if (type == 3){
        typing = true;
        isChangingRepeat = true;
        commandBuffer = "";
    }

    return false;
}

bool handleSlash(){
    slashPressed = true;

    for (WORD key : pressed) {
        switch (key){
            case 'S': {
                ShellExecuteA(NULL, "open", "spotify.exe", NULL, NULL, SW_SHOWNORMAL);
                return true;
            } 
            case 'C': {
                ShellExecuteA(NULL, "open", "chrome.exe", "--profile-directory=\"Profile 1\" https://clickup.up.ac.za", NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'F': {
                ShellExecuteA(NULL, "open", "chrome.exe", "--profile-directory=\"Profile 1\" https://ff.cs.up.ac.za/modules/", NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'B': {
                ShellExecuteA(NULL, "open", "chrome.exe", "--profile-directory=\"Default\" https://www.google.com", NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'G': {
                ShellExecuteA(NULL, "open", "chrome.exe", "--profile-directory=\"Default\" https://gemini.google.com/", NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'D': {
                ShellExecuteA(NULL, "open", "chrome.exe", "--profile-directory=\"Default\" https://discord.com/channels/@me", NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'W': {
                ShellExecuteA(NULL, "open", "chrome.exe", "--profile-directory=\"Default\" https://web.whatsapp.com/", NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'V': {
                ShellExecuteA(NULL, "open", "C:/Users/luanr/AppData/Local/Programs/Microsoft VS Code/Code.exe", NULL, NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'E': {
                ShellExecuteA(NULL, "open", "explorer.exe", "C:\\Users\\idk", NULL, SW_SHOWNORMAL);
                return true;
            }
            case 'P': {
                ShellExecuteA(NULL, "open", "powershell.exe", NULL, NULL, SW_SHOWNORMAL);
                return true;
            }
        }
    }
        
    return false;
}

bool CheckCombo(){
    slashPressed = false;

    for (WORD key : pressed){
        if (key == VK_SNAPSHOT) {
            PostQuitMessage(0);
            return true;
        }

        if (slashPressed) {
            return handleSlash();
        }

        switch (key){
            case VK_OEM_3: {
                KeyCombo({VK_LCONTROL, VK_LSHIFT, VK_OEM_3});
                return true;
            }
            case 'C':{
                std::cout << "Pressed C with copilot. Checking clipboard..." << std::endl;
                if (handleC()) return true;
            }
            case 'V':{
                if (handleV()) return true;
            }
            case VK_VOLUME_MUTE: {
                sounds = !sounds;
                return true;
            }
            case VK_VOLUME_UP: {
                volume += 0.1f;
                if (volume > 1.0f) volume = 1.0f;
                pMasterVoice->SetVolume(volume);
                return true;
            } 
            case VK_VOLUME_DOWN: {
                volume -= 0.1f;
                if (volume < 0.0f) volume = 0.0f;
                pMasterVoice->SetVolume(volume);
                return true;
            }
            case VK_UP: {
                keyReleaseSound = !keyReleaseSound;
                return true;
            }
            case VK_DOWN: {
                keyPressSound = !keyPressSound;
                return true;
            }
            case 'S':{
                commandBuffer = "";
                typing = true;
                std::cout << "Entering typing mode." << std::endl;

                return false;   // Ion want copilot to be false
            }
            case 'K':{
                keyboardMode = !keyboardMode;
                return true;
            }
            case 'M':{
                if (handleM()) return true;
            }
            case VK_OEM_2: {
                if (handleSlash()) return true;
            }
        }
    }

    return false;
}

void switchRepeatMacro(){
    repeatMacro = std::max(1, std::atoi(commandBuffer.c_str()));

    typing = false;
    commandBuffer = "";
    isChangingRepeat = false;
    copilot = false;
    pressed.clear();
    KeyRelease(VK_LSHIFT);
    KeyRelease(VK_LWIN);
}

void switchPalette(){
    std::cout << "Attempting to switch palette to: " << commandBuffer << std::endl;

    std::string path = "audio/" + commandBuffer;

    if (std::filesystem::exists(path) && std::filesystem::is_directory(path) && path != audioPath && commandBuffer != "") {
        delete pressedPalette;
        delete releasedPalette;

        pressedPalette = new Palette(path + "/press");
        releasedPalette = new Palette(path + "/release");

        audioPath = path;

        std::cout << "Switched palette to: " << commandBuffer << std::endl;
    }

    typing = false;
    commandBuffer = "";
    copilot = false;
    pressed.clear();
    KeyRelease(VK_LSHIFT);
    KeyRelease(VK_LWIN);
}

bool handleKeyDown(DWORD key) {
    if (sounds && keyPressSound && !keyPressed[key]) {
        pressedPalette->playSound(key);
        keyPressed[key] = true;
    }

    if (isRecordingMacro){
        recordMacro(key, WM_KEYDOWN);
    }

    if (keyboardMode && key == 'F' && !typing){
        mouseEvent(MOUSEEVENTF_LEFTDOWN);
        return true;
    }
    if (keyboardMode && key == 'R' && !typing){
        mouseEvent(MOUSEEVENTF_RIGHTDOWN);
        return true;
    }

    if (key == VK_F23 && !cooldown){
        KeyRelease(VK_LWIN);
        KeyRelease(VK_LSHIFT);

        if (isPlayingMacro){
            plsStop = true;
            return true;
        }

        copilot = !copilot;

        if (!copilot) {
            pressed.clear();
            slashPressed = false;
        }

        cooldown = true;

        if (typing) {
            typing = false;
            commandBuffer = "";
            isChangingRepeat = false;
            pressed.clear();
        }

        return true;
    }

    if (!copilot) return false;

    if (typing) {
        if (key == VK_RETURN) {
            if (isChangingRepeat)
                switchRepeatMacro();
            else 
                switchPalette();            
        } else if (key == VK_BACK) {
            if (!commandBuffer.empty()) commandBuffer.pop_back();
        } else if (key <= 'Z' && key >= 'A') {
            commandBuffer += (char) (key + 32);
        } else if (key >= '0' && key <= '9') {
            commandBuffer += (char) key;
        }

        return true;
    }

    pressed.push_back(key);
    if (CheckCombo()) {
        copilot = false;
        pressed.clear();
        KeyRelease(VK_LSHIFT);
        KeyRelease(VK_LWIN);
    }

    // std::cout << "Key pressed: " << key << std::endl;

    return true;
}

bool handleKeyUp(DWORD key) {
    if (sounds && keyReleaseSound && keyPressed[key]){
        releasedPalette->playSound(key);
        keyPressed[key] = false;
    }

    if (isRecordingMacro){
        recordMacro(key, WM_KEYUP);
    }

    if (keyboardMode && key == 'F' && !typing){
        mouseEvent(MOUSEEVENTF_LEFTUP);
        return true;
    }
    if (keyboardMode && key == 'R' && !typing){
        mouseEvent(MOUSEEVENTF_RIGHTUP);
        return true;
    }

    if (key == VK_F23) {
        cooldown = false;
        return true;
    }

    if (!copilot) return false;



    return true;
}

LRESULT CALLBACK process(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    // KBDLLHOOKSTRUCT *penis = (KBDLLHOOKSTRUCT *)lParam;
    // std::cout << penis->vkCode << std::endl;

    switch (wParam){
        case WM_KEYDOWN: {
            KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *)lParam;
            if (!(p->flags & LLKHF_INJECTED) && handleKeyDown(p->vkCode)) return 1;
            break;
        } case WM_KEYUP: {
            KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *)lParam;
            if (!(p->flags & LLKHF_INJECTED) && handleKeyUp(p->vkCode)) return 1;
            break;
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void init(){
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    XAudio2Create(&pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    pXAudio2->CreateMasteringVoice(&pMasterVoice);

    std::ifstream file("settings.txt");
    std::string line, value;

    std::getline(file, line);
    value = line.substr(line.find(":") + 2);
    sounds = value == "true";

    std::getline(file, line);
    value = line.substr(line.find(":") + 2);
    volume = std::stof(value);
    pMasterVoice->SetVolume(volume);

    std::getline(file, line);
    value = line.substr(line.find(":") + 2);
    keyPressSound = value == "true";

    std::getline(file, line);
    value = line.substr(line.find(":") + 2);
    keyReleaseSound = value == "true";

    std::getline(file, line);
    value = line.substr(line.find(":") + 2);
    keyboardMode = value == "true";

    std::getline(file, line);
    value = line.substr(line.find(":") + 2);
    audioPath = "audio/" + value;

    pressedPalette = new Palette(audioPath + "/press");
    releasedPalette = new Palette(audioPath + "/release");
}

void cleanup(){
    delete pressedPalette;
    delete releasedPalette;

    pMasterVoice->DestroyVoice();
    pXAudio2->Release();
    CoUninitialize();

    std::ofstream file("settings.txt");
    file << "sounds: " << (sounds ? "true" : "false") << std::endl;
    file << "volume: " << volume << std::endl;
    file << "keyPressSound: " << (keyPressSound ? "true" : "false") << std::endl;
    file << "keyReleaseSound: " << (keyReleaseSound ? "true" : "false") << std::endl;
    file << "keyboardMode: " << (keyboardMode ? "true" : "false") << std::endl;
    file << "palette: " << audioPath.substr(audioPath.find("/") + 1) << std::endl;
}

int main() {
    HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, process, 0, 0);

    init();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hook);

    cleanup();

    return 0;
}
