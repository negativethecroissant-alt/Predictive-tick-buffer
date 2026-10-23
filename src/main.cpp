#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

namespace {

constexpr float kTickRate = 240.0f;
constexpr float kTickDt = 1.0f / kTickRate; // 0.00416667s per tick (240 TPS)
constexpr int kDefaultLookaheadTicks = 480; // 480 ticks = 2.0s on 240 TPS

enum class SimObjCategory : uint8_t {
    Solid,
    Hazard,
    OrbJump,
    OrbGravity,
    OrbDash,
    PadJump,
    PadGravity,
    PortalGravityUp,
    PortalGravityDown,
    PortalGravityToggle,
    Slope
};

struct CachedEnvObject {
    cocos2d::CCRect rect;
    SimObjCategory category;
    float boostMultiplier = 1.0f;
    GameObject* rawObj = nullptr;
};

struct SimPlayerState {
    float x = 0.0f;
    float y = 0.0f;
    double yVel = 0.0;
    double gravity = 0.958199;
    float gravityMod = 1.0f;
    float speed = 0.9f;
    float vehicleSize = 1.0f;

    bool onGround = false;
    bool upsideDown = false;
    bool dead = false;

    bool isShip = false;
    bool isBird = false;
    bool isBall = false;
    bool isDart = false;
    bool isRobot = false;
    bool isSpider = false;
    bool isSwing = false;
    bool isDashing = false;
    bool holding = false;

    static SimPlayerState fromPlayer(PlayerObject* p) {
        SimPlayerState s;
        if (!p) return s;
        auto pos = p->getPosition();
        s.x = pos.x;
        s.y = pos.y;
        s.yVel = p->m_yVelocity;
        s.gravity = (p->m_gravity > 0.01) ? p->m_gravity : 0.958199;
        s.gravityMod = (p->m_gravityMod > 0.01f) ? p->m_gravityMod : 1.0f;
        s.speed = (p->m_playerSpeed > 0.05f) ? p->m_playerSpeed : 0.9f;
        s.vehicleSize = (p->m_vehicleSize > 0.1f) ? p->m_vehicleSize : 1.0f;

        s.onGround = p->m_isOnGround;
        s.upsideDown = p->m_isUpsideDown;
        s.dead = p->m_isDead;

        s.isShip = p->m_isShip;
        s.isBird = p->m_isBird;
        s.isBall = p->m_isBall;
        s.isDart = p->m_isDart;
        s.isRobot = p->m_isRobot;
        s.isSpider = p->m_isSpider;
        s.isSwing = p->m_isSwing;
        s.isDashing = p->m_isDashing;
        return s;
    }
};

struct LookaheadResult {
    bool clickRegistered = false;
    bool survivedAll = false;
    int survivedTicks = 0;
    int delayTicks = 0;
};

struct BufferedChannel {
    bool active = false;
    int targetDelayTicks = 0;
    int elapsedTicks = 0;
    int maxWindowTicks = 6;
    bool userStillHolding = false;
    bool releaseQueued = false;
    int holdTicksRemaining = 0;
    int lastAppliedDelay = 0;
    int lastSurvivedTicks = 480;

    void reset() {
        active = false;
        targetDelayTicks = 0;
        elapsedTicks = 0;
        userStillHolding = false;
        releaseQueued = false;
        holdTicksRemaining = 0;
    }
};

struct GlobalModRuntime {
    bool internalInputCall = false;
    float tickAccumulator = 0.0f;
    BufferedChannel p1;
    BufferedChannel p2;

    void resetAll() {
        internalInputCall = false;
        tickAccumulator = 0.0f;
        p1.reset();
        p2.reset();
    }
};

GlobalModRuntime g_rt;

bool isModEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

int getMaxEarlyTicks() {
    int val = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-early-ticks"));
    return std::clamp(val, 1, 16);
}

void setMaxEarlyTicks(int ticks) {
    int clamped = std::clamp(ticks, 1, 16);
    Mod::get()->setSettingValue<int64_t>("max-early-ticks", clamped);
}

int getLookaheadTicks() {
    int val = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead-ticks"));
    return std::clamp(val, 60, 960);
}

int getMinHoldTicks() {
    int val = static_cast<int>(Mod::get()->getSettingValue<int64_t>("min-hold-ticks"));
    return std::clamp(val, 1, 8);
}

bool isHudEnabled() {
    return Mod::get()->getSettingValue<bool>("show-hud");
}

bool isDualEnabled() {
    return Mod::get()->getSettingValue<bool>("dual-player");
}

// Collects nearby active objects within the 480-tick horizontal lookahead window
void gatherEnvironmentObjects(PlayLayer* pl, float startX, float maxLookaheadX, std::vector<CachedEnvObject>& out) {
    out.clear();
    if (!pl || !pl->m_objects) return;

    out.reserve(192);
    float minX = startX - 45.0f;
    float maxX = startX + maxLookaheadX + 90.0f;

    for (GameObject* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj) continue;
        auto pos = obj->getPosition();
        if (pos.x < minX || pos.x > maxX) continue;

        cocos2d::CCRect r = obj->boundingBox();
        if (r.size.width <= 0.1f || r.size.height <= 0.1f) {
            r = cocos2d::CCRect(pos.x - 15.0f, pos.y - 15.0f, 30.0f, 30.0f);
        }

        CachedEnvObject ce;
        ce.rect = r;
        ce.rawObj = obj;

        switch (obj->m_objectType) {
            case GameObjectType::Hazard:
            case GameObjectType::AnimatedHazard:
                ce.category = SimObjCategory::Hazard;
                out.push_back(ce);
                break;

            case GameObjectType::Solid:
                ce.category = SimObjCategory::Solid;
                out.push_back(ce);
                break;

            case GameObjectType::Slope:
                ce.category = SimObjCategory::Slope;
                out.push_back(ce);
                break;

            case GameObjectType::YellowJumpRing:
                ce.category = SimObjCategory::OrbJump;
                ce.boostMultiplier = 1.0f;
                out.push_back(ce);
                break;

            case GameObjectType::PinkJumpRing:
                ce.category = SimObjCategory::OrbJump;
                ce.boostMultiplier = 0.72f;
                out.push_back(ce);
                break;

            case GameObjectType::RedJumpRing:
                ce.category = SimObjCategory::OrbJump;
                ce.boostMultiplier = 1.38f;
                out.push_back(ce);
                break;

            case GameObjectType::DropRing:
                ce.category = SimObjCategory::OrbJump;
                ce.boostMultiplier = -1.25f;
                out.push_back(ce);
                break;

            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
                ce.category = SimObjCategory::OrbGravity;
                ce.boostMultiplier = (obj->m_objectType == GameObjectType::GreenRing) ? 1.0f : 0.35f;
                out.push_back(ce);
                break;

            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
                ce.category = SimObjCategory::OrbDash;
                ce.boostMultiplier = (obj->m_objectType == GameObjectType::GravityDashRing) ? -1.0f : 1.0f;
                out.push_back(ce);
                break;

            case GameObjectType::YellowJumpPad:
                ce.category = SimObjCategory::PadJump;
                ce.boostMultiplier = 1.35f;
                out.push_back(ce);
                break;

            case GameObjectType::PinkJumpPad:
                ce.category = SimObjCategory::PadJump;
                ce.boostMultiplier = 0.90f;
                out.push_back(ce);
                break;

            case GameObjectType::RedJumpPad:
                ce.category = SimObjCategory::PadJump;
                ce.boostMultiplier = 1.72f;
                out.push_back(ce);
                break;

            case GameObjectType::GravityPad:
                ce.category = SimObjCategory::PadGravity;
                out.push_back(ce);
                break;

            case GameObjectType::NormalGravityPortal:
                ce.category = SimObjCategory::PortalGravityDown;
                out.push_back(ce);
                break;

            case GameObjectType::InverseGravityPortal:
                ce.category = SimObjCategory::PortalGravityUp;
                out.push_back(ce);
                break;

            default:
                if (obj->m_objectID == 3004) {
                    ce.category = SimObjCategory::OrbGravity;
                    ce.boostMultiplier = 0.35f;
                    out.push_back(ce);
                } else if (obj->m_objectID == 3005) {
                    ce.category = SimObjCategory::PadGravity;
                    out.push_back(ce);
                } else if (obj->m_objectID == 2926) {
                    ce.category = SimObjCategory::PortalGravityToggle;
                    out.push_back(ce);
                }
                break;
        }
    }
}

void stepSingle240HzTick(
    SimPlayerState& s,
    bool pressThisTick,
    std::vector<CachedEnvObject> const& env,
    bool& outClickRegistered
) {
    if (s.dead) return;

    float const gravSign = s.upsideDown ? -1.0f : 1.0f;
    float const halfSize = 14.0f * s.vehicleSize;
    cocos2d::CCRect playerRect(s.x - halfSize, s.y - halfSize, halfSize * 2.0f, halfSize * 2.0f);

    if (pressThisTick) {
        s.holding = true;
        bool activatedOrb = false;

        for (auto const& obj : env) {
            if (!playerRect.intersectsRect(obj.rect)) continue;

            if (obj.category == SimObjCategory::OrbJump) {
                s.yVel = static_cast<double>(gravSign * 11.18f * obj.boostMultiplier * s.vehicleSize);
                s.onGround = false;
                activatedOrb = true;
                outClickRegistered = true;
                break;
            } else if (obj.category == SimObjCategory::OrbGravity) {
                s.upsideDown = !s.upsideDown;
                float newGravSign = s.upsideDown ? -1.0f : 1.0f;
                s.yVel = static_cast<double>(newGravSign * 9.8f * obj.boostMultiplier * s.vehicleSize);
                s.onGround = false;
                activatedOrb = true;
                outClickRegistered = true;
                break;
            } else if (obj.category == SimObjCategory::OrbDash) {
                if (obj.boostMultiplier < 0.0f) {
                    s.upsideDown = !s.upsideDown;
                }
                s.isDashing = true;
                s.yVel = 0.0;
                s.onGround = false;
                activatedOrb = true;
                outClickRegistered = true;
                break;
            }
        }

        if (!activatedOrb) {
            if (s.isShip || s.isDart || s.isBird || s.isSwing) {
                outClickRegistered = true;
                if (s.isBird) {
                    s.yVel = static_cast<double>(gravSign * 7.65f * s.vehicleSize);
                    s.onGround = false;
                } else if (s.isSwing) {
                    s.upsideDown = !s.upsideDown;
                    s.onGround = false;
                }
            } else if (s.isBall) {
                if (s.onGround) {
                    s.upsideDown = !s.upsideDown;
                    s.yVel = static_cast<double>((s.upsideDown ? -1.0f : 1.0f) * -2.0f);
                    s.onGround = false;
                    outClickRegistered = true;
                }
            } else if (s.isSpider) {
                if (s.onGround) {
                    s.upsideDown = !s.upsideDown;
                    s.yVel = 0.0;
                    float spiderDir = s.upsideDown ? 1.0f : -1.0f;
                    float targetY = s.y + spiderDir * 240.0f;
                    for (auto const& obj : env) {
                        if (obj.category != SimObjCategory::Solid) continue;
                        if (obj.rect.getMaxX() < s.x - halfSize || obj.rect.getMinX() > s.x + halfSize) continue;
                        if (spiderDir > 0.0f && obj.rect.getMinY() > s.y) {
                            targetY = std::min(targetY, obj.rect.getMinY() - halfSize);
                        } else if (spiderDir < 0.0f && obj.rect.getMaxY() < s.y) {
                            targetY = std::max(targetY, obj.rect.getMaxY() + halfSize);
                        }
                    }
                    s.y = targetY;
                    s.onGround = true;
                    outClickRegistered = true;
                }
            } else {
                if (s.onGround) {
                    s.yVel = static_cast<double>(gravSign * 11.18f * s.vehicleSize);
                    s.onGround = false;
                    outClickRegistered = true;
                }
            }
        }
    }

    float const dx = 311.58f * s.speed * kTickDt;
    s.x += dx;

    float const curGravSign = s.upsideDown ? -1.0f : 1.0f;
    if (s.isDashing) {
        if (!s.holding) s.isDashing = false;
    } else if (s.isDart) {
        float waveSlope = (s.holding ? 1.0f : -1.0f) * curGravSign * (s.vehicleSize < 0.8f ? 2.0f : 1.0f);
        s.y += dx * waveSlope;
        s.yVel = waveSlope * 10.0;
    } else if (s.isShip) {
        double accel = s.holding ? (curGravSign * 0.145) : (-curGravSign * 0.115);
        s.yVel = std::clamp(s.yVel + accel, -12.0, 12.0);
        s.y += static_cast<float>(s.yVel * 60.0 * kTickDt * 0.9);
    } else {
        if (!s.onGround) {
            s.yVel -= curGravSign * s.gravity * s.gravityMod * 0.24;
            s.yVel = std::clamp(s.yVel, -16.0, 16.0);
            s.y += static_cast<float>(s.yVel * 60.0 * kTickDt * 0.92);
        } else {
            s.onGround = false;
        }
    }

    if (s.y <= 15.0f * s.vehicleSize && !s.upsideDown) {
        s.y = 15.0f * s.vehicleSize;
        s.yVel = 0.0;
        s.onGround = true;
    }
    if (s.y < -120.0f || s.y > 2600.0f) {
        s.dead = true;
        return;
    }

    cocos2d::CCRect newRect(s.x - halfSize, s.y - halfSize, halfSize * 2.0f, halfSize * 2.0f);
    cocos2d::CCRect innerHitbox(s.x - halfSize * 0.45f, s.y - halfSize * 0.45f, halfSize * 0.9f, halfSize * 0.9f);

    for (auto const& obj : env) {
        if (obj.rect.getMaxX() < newRect.getMinX() || obj.rect.getMinX() > newRect.getMaxX()) continue;
        if (!newRect.intersectsRect(obj.rect)) continue;

        switch (obj.category) {
            case SimObjCategory::Hazard:
                if (innerHitbox.intersectsRect(obj.rect)) {
                    s.dead = true;
                    return;
                }
                break;

            case SimObjCategory::Solid: {
                float topTol = 8.5f;
                if (!s.upsideDown && s.yVel <= 0.05 && (s.y - halfSize) >= (obj.rect.getMaxY() - topTol)) {
                    s.y = obj.rect.getMaxY() + halfSize;
                    s.yVel = 0.0;
                    s.onGround = true;
                } else if (s.upsideDown && s.yVel >= -0.05 && (s.y + halfSize) <= (obj.rect.getMinY() + topTol)) {
                    s.y = obj.rect.getMinY() - halfSize;
                    s.yVel = 0.0;
                    s.onGround = true;
                } else if (innerHitbox.intersectsRect(obj.rect)) {
                    s.dead = true;
                    return;
                }
                break;
            }

            case SimObjCategory::Slope:
                if (!s.upsideDown && s.y < obj.rect.getMaxY() + halfSize) {
                    s.y = obj.rect.getMaxY() + halfSize;
                    s.yVel = std::max(0.0, s.yVel);
                    s.onGround = true;
                }
                break;

            case SimObjCategory::PadJump:
                s.yVel = static_cast<double>(curGravSign * 11.18f * obj.boostMultiplier * s.vehicleSize);
                s.onGround = false;
                break;

            case SimObjCategory::PadGravity:
                s.upsideDown = !s.upsideDown;
                s.yVel = static_cast<double>((s.upsideDown ? -1.0f : 1.0f) * -8.5f);
                s.onGround = false;
                break;

            case SimObjCategory::PortalGravityUp:
                s.upsideDown = true;
                break;

            case SimObjCategory::PortalGravityDown:
                s.upsideDown = false;
                break;

            case SimObjCategory::PortalGravityToggle:
                s.upsideDown = !s.upsideDown;
                break;

            default:
                break;
        }
    }
}

LookaheadResult simulateCandidateDelay(
    SimPlayerState const& initialState,
    int delayTicks,
    int lookaheadTicks,
    std::vector<CachedEnvObject> const& env
) {
    LookaheadResult res;
    res.delayTicks = delayTicks;

    SimPlayerState state = initialState;
    state.holding = false;

    for (int t = 0; t < delayTicks; ++t) {
        bool dummyReg = false;
        stepSingle240HzTick(state, false, env, dummyReg);
        if (state.dead) {
            res.survivedAll = false;
            res.survivedTicks = 0;
            return res;
        }
    }

    int holdWindow = std::max(2, getMinHoldTicks());
    for (int t = 0; t < lookaheadTicks; ++t) {
        bool pressNow = (t == 0);
        if (t >= holdWindow && !state.isShip && !state.isDart) {
            state.holding = false;
        }
        stepSingle240HzTick(state, pressNow, env, res.clickRegistered);
        if (state.dead) {
            res.survivedAll = false;
            res.survivedTicks = t;
            return res;
        }
    }

    res.survivedAll = true;
    res.survivedTicks = lookaheadTicks;
    return res;
}

LookaheadResult findOptimalClickDelay(PlayerObject* player, int maxEarlyTicks, int lookaheadTicks) {
    LookaheadResult fallback{true, true, lookaheadTicks, 0};
    auto* pl = PlayLayer::get();
    if (!pl || !player) return fallback;

    SimPlayerState baseState = SimPlayerState::fromPlayer(player);
    float maxLookaheadX = 311.58f * baseState.speed * static_cast<float>(maxEarlyTicks + lookaheadTicks) * kTickDt;

    std::vector<CachedEnvObject> env;
    gatherEnvironmentObjects(pl, baseState.x, maxLookaheadX, env);

    LookaheadResult immediate = simulateCandidateDelay(baseState, 0, lookaheadTicks, env);
    if (immediate.clickRegistered && immediate.survivedAll) {
        return immediate;
    }

    LookaheadResult best = immediate;
    for (int d = 1; d <= maxEarlyTicks; ++d) {
        LookaheadResult cand = simulateCandidateDelay(baseState, d, lookaheadTicks, env);
        if (cand.clickRegistered && cand.survivedAll) {
            return cand;
        }
        if (cand.clickRegistered && (!best.clickRegistered || cand.survivedTicks > best.survivedTicks)) {
            best = cand;
        }
    }

    return best;
}

void updateHudDisplay() {
    auto* pl = PlayLayer::get();
    if (!pl) return;
    auto* hudLabel = typeinfo_cast<cocos2d::CCLabelBMFont*>(pl->getChildByID("tick-buffer-hud"_spr));
    if (!hudLabel) return;

    if (!isModEnabled() || !isHudEnabled()) {
        hudLabel->setVisible(false);
        return;
    }
    hudLabel->setVisible(true);
    int windowTicks = getMaxEarlyTicks();
    int lookahead = getLookaheadTicks();
    if (g_rt.p1.active) {
        hudLabel->setString(
            fmt::format("TickBuffer [1-{}t]: Buffering {}/{}t (240 TPS)", windowTicks, g_rt.p1.elapsedTicks, g_rt.p1.targetDelayTicks).c_str()
        );
        hudLabel->setColor({255, 230, 80});
    } else {
        hudLabel->setString(
            fmt::format("TickBuffer [1-{}t]: Last +{}t | {}t/{}t OK", windowTicks, g_rt.p1.lastAppliedDelay, g_rt.p1.lastSurvivedTicks, lookahead).c_str()
        );
        hudLabel->setColor({130, 255, 150});
    }
}

class TickBufferPopup : public geode::Popup {
protected:
    cocos2d::CCLabelBMFont* m_valueLabel = nullptr;
    cocos2d::CCLabelBMFont* m_msLabel = nullptr;
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    std::vector<CCMenuItemSpriteExtra*> m_tickButtons;

    bool initPopup() {
        if (!Popup::init(360.f, 250.f)) return false;

        this->setTitle("240 TPS Early Click Buffer (1-16 Ticks)");

        auto winSize = m_mainLayer->getContentSize();

        auto subLabel = CCLabelBMFont::create(
            "Buffers early clicks by 1-16 ticks until 480-tick check passes",
            "chatFont.fnt"
        );
        subLabel->setScale(0.62f);
        subLabel->setColor({200, 220, 255});
        subLabel->setPosition({winSize.width / 2.f, winSize.height - 42.f});
        m_mainLayer->addChild(subLabel);

        m_valueLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_valueLabel->setScale(0.75f);
        m_valueLabel->setPosition({winSize.width / 2.f, winSize.height - 70.f});
        m_mainLayer->addChild(m_valueLabel);

        m_msLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_msLabel->setScale(0.58f);
        m_msLabel->setColor({160, 255, 180});
        m_msLabel->setPosition({winSize.width / 2.f, winSize.height - 88.f});
        m_mainLayer->addChild(m_msLabel);

        auto stepMenu = CCMenu::create();
        stepMenu->setPosition({winSize.width / 2.f, winSize.height - 70.f});
        m_mainLayer->addChild(stepMenu);

        auto addStepBtn = [&](char const* text, int delta, float xOffset) {
            auto spr = ButtonSprite::create(text, "bigFont.fnt", "GJ_button_01.png", 0.6f);
            spr->setScale(0.58f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(TickBufferPopup::onStepTicks));
            btn->setTag(delta);
            btn->setPosition({xOffset, 0.f});
            stepMenu->addChild(btn);
        };

        addStepBtn("-4", -4, -135.f);
        addStepBtn("-1", -1, -92.f);
        addStepBtn("+1", 1, 92.f);
        addStepBtn("+4", 4, 135.f);

        auto gridMenu = CCMenu::create();
        gridMenu->setPosition({winSize.width / 2.f, winSize.height / 2.f - 8.f});
        m_mainLayer->addChild(gridMenu);

        m_tickButtons.clear();
        for (int t = 1; t <= 16; ++t) {
            int row = (t - 1) / 8;
            int col = (t - 1) % 8;
            float x = (static_cast<float>(col) - 3.5f) * 38.0f;
            float y = (row == 0) ? 16.0f : -20.0f;

            auto spr = ButtonSprite::create(
                fmt::format("{}", t).c_str(),
                "bigFont.fnt",
                "GJ_button_04.png",
                0.55f
            );
            spr->setScale(0.52f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(TickBufferPopup::onSelectExactTick));
            btn->setTag(t);
            btn->setPosition({x, y});
            gridMenu->addChild(btn);
            m_tickButtons.push_back(btn);
        }

        auto bottomMenu = CCMenu::create();
        bottomMenu->setPosition({winSize.width / 2.f, 34.f});
        m_mainLayer->addChild(bottomMenu);

        auto toggleSpr = ButtonSprite::create("Toggle Mod", "goldFont.fnt", "GJ_button_01.png", 0.65f);
        toggleSpr->setScale(0.6f);
        auto toggleBtn = CCMenuItemSpriteExtra::create(toggleSpr, this, menu_selector(TickBufferPopup::onToggleEnabled));
        toggleBtn->setPosition({-100.f, 0.f});
        bottomMenu->addChild(toggleBtn);

        auto hudSpr = ButtonSprite::create("Toggle HUD", "goldFont.fnt", "GJ_button_02.png", 0.65f);
        hudSpr->setScale(0.6f);
        auto hudBtn = CCMenuItemSpriteExtra::create(hudSpr, this, menu_selector(TickBufferPopup::onToggleHud));
        hudBtn->setPosition({0.f, 0.f});
        bottomMenu->addChild(hudBtn);

        auto settingsSpr = ButtonSprite::create("All Settings", "goldFont.fnt", "GJ_button_04.png", 0.65f);
        settingsSpr->setScale(0.6f);
        auto settingsBtn = CCMenuItemSpriteExtra::create(settingsSpr, this, menu_selector(TickBufferPopup::onOpenModSettings));
        settingsBtn->setPosition({105.f, 0.f});
        bottomMenu->addChild(settingsBtn);

        m_statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_statusLabel->setScale(0.58f);
        m_statusLabel->setPosition({winSize.width / 2.f, 62.f});
        m_mainLayer->addChild(m_statusLabel);

        this->refreshUI();
        return true;
    }

    void refreshUI() {
        int ticks = getMaxEarlyTicks();
        float ms = static_cast<float>(ticks) * (1000.0f / kTickRate);
        bool enabled = isModEnabled();
        bool hud = isHudEnabled();

        if (m_valueLabel) {
            m_valueLabel->setString(fmt::format("Window: {} {}", ticks, ticks == 1 ? "Tick" : "Ticks").c_str());
        }
        if (m_msLabel) {
            m_msLabel->setString(
                fmt::format("{:.2f} ms @ 240 TPS | 480-Tick Survival Check (2.00s)", ms).c_str()
            );
        }
        if (m_statusLabel) {
            m_statusLabel->setString(
                fmt::format("Mod: {}   |   Live HUD: {}", enabled ? "ENABLED" : "DISABLED", hud ? "ON" : "OFF").c_str()
            );
            m_statusLabel->setColor(enabled ? cocos2d::ccColor3B{120, 255, 140} : cocos2d::ccColor3B{255, 110, 110});
        }

        for (auto* btn : m_tickButtons) {
            if (!btn) continue;
            bool selected = (btn->getTag() == ticks);
            if (auto* spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                spr->setColor(selected ? cocos2d::ccColor3B{100, 255, 140} : cocos2d::ccColor3B{200, 200, 200});
                spr->setScale(selected ? 0.60f : 0.50f);
            }
        }

        updateHudDisplay();
    }

    void onStepTicks(CCObject* sender) {
        int delta = sender->getTag();
        setMaxEarlyTicks(getMaxEarlyTicks() + delta);
        this->refreshUI();
    }

    void onSelectExactTick(CCObject* sender) {
        int tick = sender->getTag();
        setMaxEarlyTicks(tick);
        this->refreshUI();
    }

    void onToggleEnabled(CCObject*) {
        bool next = !isModEnabled();
        Mod::get()->setSettingValue<bool>("enabled", next);
        if (!next) g_rt.resetAll();
        this->refreshUI();
    }

    void onToggleHud(CCObject*) {
        bool next = !isHudEnabled();
        Mod::get()->setSettingValue<bool>("show-hud", next);
        this->refreshUI();
    }

    void onOpenModSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }

public:
    static TickBufferPopup* create() {
        auto ret = new TickBufferPopup();
        if (ret->initPopup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void fireBufferedClickNow(PlayerObject* player, BufferedChannel& ch) {
    if (!player || !ch.active) return;

    ch.active = false;
    ch.lastAppliedDelay = ch.elapsedTicks;

    g_rt.internalInputCall = true;
    player->pushButton(PlayerButton::Jump);
    g_rt.internalInputCall = false;

    if (ch.releaseQueued || !ch.userStillHolding) {
        ch.holdTicksRemaining = getMinHoldTicks();
        ch.releaseQueued = false;
    } else {
        ch.holdTicksRemaining = 0;
    }

    updateHudDisplay();
}

void stepChannelOn240HzTick(PlayerObject* player, BufferedChannel& ch) {
    if (!player) return;

    if (!ch.active && ch.holdTicksRemaining > 0) {
        ch.holdTicksRemaining--;
        if (ch.holdTicksRemaining <= 0 && !ch.userStillHolding) {
            g_rt.internalInputCall = true;
            player->releaseButton(PlayerButton::Jump);
            g_rt.internalInputCall = false;
        }
    }

    if (!ch.active) return;

    ch.elapsedTicks++;

    int remainingWindow = std::max(0, ch.maxWindowTicks - ch.elapsedTicks);
    LookaheadResult nowCheck = findOptimalClickDelay(player, remainingWindow, getLookaheadTicks());

    if ((nowCheck.delayTicks == 0 && nowCheck.clickRegistered && nowCheck.survivedAll) ||
        ch.elapsedTicks >= ch.targetDelayTicks ||
        ch.elapsedTicks >= ch.maxWindowTicks) {
        ch.lastSurvivedTicks = nowCheck.survivedTicks;
        fireBufferedClickNow(player, ch);
    } else {
        updateHudDisplay();
    }
}

} // namespace

class $modify(BufferedPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton button) {
        if (g_rt.internalInputCall || !isModEnabled() || button != PlayerButton::Jump) {
            return PlayerObject::pushButton(button);
        }

        auto* pl = PlayLayer::get();
        if (!pl || this->m_isDead) {
            return PlayerObject::pushButton(button);
        }

        bool isP1 = (this == pl->m_player1);
        bool isP2 = (this == pl->m_player2);
        if (!isP1 && (!isP2 || !isDualEnabled())) {
            return PlayerObject::pushButton(button);
        }

        BufferedChannel& ch = isP1 ? g_rt.p1 : g_rt.p2;
        ch.userStillHolding = true;
        ch.releaseQueued = false;
        ch.holdTicksRemaining = 0;

        int maxEarlyTicks = getMaxEarlyTicks();
        int lookaheadTicks = getLookaheadTicks();

        LookaheadResult eval = findOptimalClickDelay(this, maxEarlyTicks, lookaheadTicks);

        if (eval.delayTicks <= 0) {
            ch.active = false;
            ch.lastAppliedDelay = 0;
            ch.lastSurvivedTicks = eval.survivedTicks;
            updateHudDisplay();
            return PlayerObject::pushButton(button);
        }

        ch.active = true;
        ch.targetDelayTicks = std::clamp(eval.delayTicks, 1, maxEarlyTicks);
        ch.elapsedTicks = 0;
        ch.maxWindowTicks = maxEarlyTicks;
        ch.lastSurvivedTicks = eval.survivedTicks;

        updateHudDisplay();
        return true;
    }

    bool releaseButton(PlayerButton button) {
        if (g_rt.internalInputCall || !isModEnabled() || button != PlayerButton::Jump) {
            return PlayerObject::releaseButton(button);
        }

        auto* pl = PlayLayer::get();
        if (!pl) {
            return PlayerObject::releaseButton(button);
        }

        bool isP1 = (this == pl->m_player1);
        bool isP2 = (this == pl->m_player2);
        if (!isP1 && (!isP2 || !isDualEnabled())) {
            return PlayerObject::releaseButton(button);
        }

        BufferedChannel& ch = isP1 ? g_rt.p1 : g_rt.p2;
        ch.userStillHolding = false;

        if (ch.active) {
            ch.releaseQueued = true;
            return true;
        }

        if (ch.holdTicksRemaining > 0) {
            return true;
        }

        return PlayerObject::releaseButton(button);
    }
};

class $modify(BufferedPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_rt.resetAll();

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto hud = CCLabelBMFont::create("TickBuffer: Ready", "chatFont.fnt");
        hud->setScale(0.55f);
        hud->setAnchorPoint({0.0f, 0.5f});
        hud->setPosition({12.0f, winSize.height - 26.0f});
        hud->setOpacity(210);
        hud->setZOrder(999);
        hud->setID("tick-buffer-hud"_spr);
        this->addChild(hud);

        updateHudDisplay();
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (!isModEnabled()) return;

        float clampedDt = std::clamp(dt, 0.0f, 0.1f);
        g_rt.tickAccumulator += clampedDt;

        int steps = 0;
        while (g_rt.tickAccumulator >= kTickDt && steps < 16) {
            g_rt.tickAccumulator -= kTickDt;
            steps++;

            if (this->m_player1 && !this->m_player1->m_isDead) {
                stepChannelOn240HzTick(this->m_player1, g_rt.p1);
            } else if (this->m_player1 && this->m_player1->m_isDead && g_rt.p1.active) {
                g_rt.p1.reset();
            }

            if (isDualEnabled() && this->m_player2 && !this->m_player2->m_isDead) {
                stepChannelOn240HzTick(this->m_player2, g_rt.p2);
            } else if (this->m_player2 && this->m_player2->m_isDead && g_rt.p2.active) {
                g_rt.p2.reset();
            }
        }
    }
};

class $modify(BufferedPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto btnSpr = ButtonSprite::create("1-16t", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        btnSpr->setScale(0.65f);

        auto btn = CCMenuItemSpriteExtra::create(
            btnSpr,
            this,
            menu_selector(BufferedPauseLayer::onOpenTickBufferPopup)
        );
        btn->setID("open-tick-buffer-btn"_spr);

        if (auto* leftMenu = this->getChildByID("left-button-menu")) {
            leftMenu->addChild(btn);
            leftMenu->updateLayout();
        } else {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            auto fallbackMenu = CCMenu::create();
            fallbackMenu->setPosition({42.f, winSize.height / 2.f});
            fallbackMenu->addChild(btn);
            fallbackMenu->setID("tick-buffer-pause-menu"_spr);
            this->addChild(fallbackMenu);
        }
    }

    void onOpenTickBufferPopup(CCObject*) {
        TickBufferPopup::create()->show();
    }
};

$on_mod(Loaded) {
    listenForSettingChanges<int64_t>("max-early-ticks", [](int64_t) {
        updateHudDisplay();
    });
    listenForSettingChanges<bool>("enabled", [](bool enabled) {
        if (!enabled) g_rt.resetAll();
        updateHudDisplay();
    });
    listenForSettingChanges<bool>("show-hud", [](bool) {
        updateHudDisplay();
    });
}
