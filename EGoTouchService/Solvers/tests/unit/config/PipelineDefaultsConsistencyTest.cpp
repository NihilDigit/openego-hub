// Guards the invariant that a freshly constructed pipeline already holds the values
// registerBindings() declares as defaults.
//
// Why this matters: the binder default is what the schema advertises and what a reset,
// a validation report or the diagnostics UI shows, while the member initializer is what
// actually executes on any field applyConfig() does not reach — no config file, a store
// rejected by validation, or a key that was never bound. When the two disagree, the
// advertised default and the running value are different algorithms, and the discrepancy
// only surfaces as behaviour nobody can trace back to a setting.

#include "StylusSolver/StylusPipeline.h"
#include "TouchSolver/TouchPipeline.h"
#include "config/ConfigBinder.h"
#include "config/ConfigSchemaSnapshot.h"
#include "config/ConfigStore.h"
#include "config/ConfigValue.h"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Drift {
    std::string yamlPath;
    std::string declaredDefault;
    std::string memberValue;
};

std::vector<Drift> CollectDrift(const Config::ConfigSchemaSnapshot& schema) {
    std::vector<Drift> drift;
    for (const auto& entry : schema.entries) {
        // Schema-only keys have no member behind them, so there is nothing to compare.
        if (!entry.boundToRuntime) {
            continue;
        }
        if (entry.defaultValue == entry.currentValue) {
            continue;
        }
        drift.push_back({entry.yamlPath,
                         Config::toString(entry.defaultValue),
                         Config::toString(entry.currentValue)});
    }
    return drift;
}

// applyConfig() 为每个键各自写了一份回退值，那是配置里缺这个键时实际生效的数字。它和
// binder 默认、成员初值是三份独立的字面量，而上面那个检查只覆盖后两者——真发生过
// touch.palm_box.expand_rows 成员和 binder 都是 9、applyConfig 回退写成 1 的情况，
// 掌框外扩因此缩到九分之一，而三项里有两项一致，测试全绿。
//
// 空 store 走一遍 applyConfig，凡是回退值和成员初值不一致的键都会在这里露出来。
std::vector<Drift> CollectFallbackDrift(Config::ConfigBinder& binder,
                                        Solvers::TouchPipeline& touch,
                                        Solvers::StylusPipeline& stylus) {
    std::map<std::string, std::string> before;
    for (const auto& entry : binder.snapshot().entries) {
        if (entry.boundToRuntime) {
            before[entry.yamlPath] = Config::toString(entry.currentValue);
        }
    }

    const Config::ConfigStore empty;
    touch.applyConfig(empty);
    stylus.applyConfig(empty);

    std::vector<Drift> drift;
    for (const auto& entry : binder.snapshot().entries) {
        if (!entry.boundToRuntime) continue;
        const auto it = before.find(entry.yamlPath);
        if (it == before.end()) continue;
        const std::string after = Config::toString(entry.currentValue);
        if (it->second == after) continue;
        drift.push_back({entry.yamlPath, it->second, after});
    }
    return drift;
}

} // namespace

int main() {
    try {
        Config::ConfigBinder binder;
        Solvers::TouchPipeline touch;
        Solvers::StylusPipeline stylus;
        touch.registerBindings(binder);
        stylus.registerBindings(binder);

        const auto drift = CollectDrift(binder.snapshot());
        if (!drift.empty()) {
            std::cerr << "Pipeline member initializers disagree with registerBindings() defaults.\n"
                         "These keys advertise one default and run another whenever config\n"
                         "injection does not reach them:\n\n";
            for (const auto& d : drift) {
                std::cerr << "  " << d.yamlPath
                          << "\n      binder default : " << d.declaredDefault
                          << "\n      member value   : " << d.memberValue << "\n";
            }
            std::cerr << "\n" << drift.size() << " key(s) drifted.\n";
            return 1;
        }

        const auto fallbackDrift = CollectFallbackDrift(binder, touch, stylus);
        if (!fallbackDrift.empty()) {
            std::cerr << "applyConfig() fallbacks disagree with the member initializers.\n"
                         "These keys run a different value whenever the config store does\n"
                         "not carry them:\n\n";
            for (const auto& d : fallbackDrift) {
                std::cerr << "  " << d.yamlPath
                          << "\n      member value      : " << d.declaredDefault
                          << "\n      applyConfig value : " << d.memberValue << "\n";
            }
            std::cerr << "\n" << fallbackDrift.size() << " key(s) drifted.\n";
            return 1;
        }

        std::cout << "All bound pipeline defaults agree with their member initializers.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "unexpected exception: " << ex.what() << "\n";
        return 1;
    }
}
