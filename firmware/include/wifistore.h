// Сохранённые сети Wi-Fi в NVS (как настройки в settings.h).
//
// Пароли лежат во внутренней памяти открытым текстом: её не вынешь из платы,
// в отличие от SD-карты. Сеть из secrets.h сюда не пишем — она запасная, в прошивке.
#pragma once
#include <Preferences.h>
#include <vector>

struct WifiNet {
    String ssid, pass;
};

class WifiStore {
public:
    static constexpr int MAX = 5;

    void load() {
        nets_.clear();
        Preferences p;
        p.begin("wifi", false);
        int n = min((int)p.getUChar("n", 0), MAX);
        for (int i = 0; i < n; i++) {
            nets_.push_back(WifiNet{p.getString(key("s", i).c_str(), ""), p.getString(key("p", i).c_str(), "")});
        }
        p.end();
    }

    const std::vector<WifiNet>& nets() const { return nets_; }

    // Последняя сеть, к которой подключились (первая в списке), или nullptr
    const WifiNet* last() const { return nets_.empty() ? nullptr : &nets_[0]; }

    const WifiNet* find(const String& ssid) const {
        for (const WifiNet& n : nets_) {
            if (n.ssid == ssid) return &n;
        }
        return nullptr;
    }

    // Подключились: сеть — первой в списке (свежие сверху), старая копия убирается.
    // Больше MAX — самая давняя забывается
    void remember(const String& ssid, const String& pass) {
        forgetInMemory(ssid);
        nets_.insert(nets_.begin(), WifiNet{ssid, pass});
        if ((int)nets_.size() > MAX) nets_.pop_back();
        save();
    }

    void forget(const String& ssid) {
        forgetInMemory(ssid);
        save();
    }

private:
    static String key(const char* prefix, int i) { return String(prefix) + i; }

    void forgetInMemory(const String& ssid) {
        for (size_t i = 0; i < nets_.size(); i++) {
            if (nets_[i].ssid == ssid) {
                nets_.erase(nets_.begin() + i);
                return;
            }
        }
    }

    void save() {
        Preferences p;
        p.begin("wifi", false);
        p.clear();                       // список целиком: проще, чем сдвигать ключи
        p.putUChar("n", nets_.size());
        for (size_t i = 0; i < nets_.size(); i++) {
            p.putString(key("s", i).c_str(), nets_[i].ssid);
            p.putString(key("p", i).c_str(), nets_[i].pass);
        }
        p.end();
    }

    std::vector<WifiNet> nets_;
};
