#pragma once

#include "nightglass/core/status.hpp"

namespace nightglass::core {

struct AppContext;
struct Event;

class IApp {
public:
    virtual ~IApp() = default;
    virtual Status create(AppContext &context) = 0;
    virtual void enter() = 0;
    virtual void on_event(const Event &event) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void exit() = 0;
    virtual void destroy_view() = 0;
};

}  // namespace nightglass::core
