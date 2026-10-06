// Каркас навигации: экраны и стек экранов.
//
// Экран навигации — это меню, список или вопрос Да/Нет. Состояния агента
// (слушаю, думаю, карточка «Разрешить?», ответ) сюда не входят: ими управляет
// сервер, и они живут в Mode в main.cpp.
//
// Стек: push открывает экран поверх, pop возвращает на шаг назад. Так «назад»
// всегда работает одинаково, и экрану не нужно знать, откуда его открыли.
#pragma once
#include <vector>

class Screen {
public:
    virtual ~Screen() = default;
    virtual void onOpen() {}            // экран открыли заново (push), а не вернулись к нему
    virtual void draw() = 0;
    virtual void onRotate(int dir) {}
    virtual void onClick() {}
    // true — экран сам обработал удержание. false — удержание значит «говорить»
    virtual bool onHold() { return false; }
    virtual bool showsFace() const { return false; }
    // Зовётся в каждом loop(), пока экран сверху: проверить фоновую работу (поиск сетей и т.п.)
    virtual void tick() {}
};

class ScreenStack {
public:
    bool empty() const { return items_.empty(); }
    Screen* top() const { return items_.empty() ? nullptr : items_.back(); }
    void push(Screen* s) { items_.push_back(s); }
    void pop() {
        if (!items_.empty()) items_.pop_back();
    }
    void clear() { items_.clear(); }

private:
    std::vector<Screen*> items_;
};
