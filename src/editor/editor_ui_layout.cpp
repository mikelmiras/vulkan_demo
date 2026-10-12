// Editor: game UI layouts (UIElement, game/ui_layout.h): adding elements, their inspector, and the preview over the
// viewport where they're picked and dragged into place.
#include "editor/editor.h"

#include <algorithm>
#include <cfloat>

#include "editor/editor_ui.h"

using namespace editor_ui;

void Editor::addUIElement(UIElement::Kind kind) {
    pushUndo();
    // Under the selected UI element; or in a canvas (the whole screen) made for it
    EntityId parent = kNoEntity;
    if (const Entity* sel = selection(); sel && sel->ui) parent = sel->id;
    if (parent == kNoEntity && kind != UIElement::Kind::Panel) {
        Entity& canvas = scene_.create(scene_.uniqueName("Canvas"));
        canvas.ui = UIElement{};
        canvas.ui->anchorMin[0] = canvas.ui->anchorMin[1] = 0.f;
        canvas.ui->anchorMax[0] = canvas.ui->anchorMax[1] = 1.f;
        canvas.ui->offsetMin[0] = canvas.ui->offsetMin[1] = canvas.ui->offsetMax[0] = canvas.ui->offsetMax[1] = 0.f;
        canvas.ui->opacity = 0.f;
        parent = canvas.id;
    }
    static const char* names[] = {"Panel", "Text", "Image", "Button", "Slider", "Toggle", "Text Field", "List"};
    Entity& e = scene_.create(scene_.uniqueName(names[(int)kind]), parent);
    UIElement u;
    u.kind = kind;
    switch (kind) {
        case UIElement::Kind::Text: u.text = "Text"; u.opacity = 0.f; break;
        case UIElement::Kind::Button: u.text = "Button"; u.offsetMin[1] = -35.f; u.offsetMax[1] = 35.f; break;
        case UIElement::Kind::Slider: u.text = "Volume"; u.value = "volume"; u.offsetMin[0] = -250.f; u.offsetMax[0] = 250.f; break;
        case UIElement::Kind::Toggle: u.text = "Option"; u.value = "option"; break;
        case UIElement::Kind::TextField: u.text = "Your name"; u.value = "name"; u.offsetMin[0] = -250.f; u.offsetMax[0] = 250.f; break;
        case UIElement::Kind::Image: u.image = "Checker"; u.opacity = 1.f; u.offsetMin[1] = -150.f; u.offsetMax[1] = 150.f; break;
        case UIElement::Kind::List: u.offsetMin[1] = -200.f; u.offsetMax[1] = 200.f; u.opacity = 0.f; break;
        default: break;
    }
    e.ui = u;
    select(e.id);
    showGameUi_ = true;
    setStatus(std::string("Added ") + names[(int)kind] + ": drag it in the viewport, or set its anchors in the Inspector");
}

void Editor::drawUIInspector(Entity& e) {
    UIElement& u = *e.ui;
    const char* kinds[] = {"Panel", "Text", "Image", "Button", "Slider", "Toggle", "Text field", "List"};
    int kind = (int)u.kind;
    if (ImGui::Combo("Kind", &kind, kinds, 8)) { pushUndo(); u.kind = (UIElement::Kind)kind; }
    // Anchors: presets, then the numbers
    struct Preset { const char* name; float a0x, a0y, a1x, a1y; };
    static const Preset presets[] = {{"Top left", 0, 0, 0, 0},      {"Top", 0.5f, 0, 0.5f, 0},     {"Top right", 1, 0, 1, 0},
                                     {"Left", 0, 0.5f, 0, 0.5f},    {"Centre", 0.5f, 0.5f, 0.5f, 0.5f}, {"Right", 1, 0.5f, 1, 0.5f},
                                     {"Bottom left", 0, 1, 0, 1},   {"Bottom", 0.5f, 1, 0.5f, 1},  {"Bottom right", 1, 1, 1, 1},
                                     {"Stretch across", 0, 0.5f, 1, 0.5f}, {"Stretch down", 0.5f, 0, 0.5f, 1},
                                     {"Fill", 0, 0, 1, 1}};
    if (ImGui::BeginCombo("Anchor", "(preset)")) {
        for (const Preset& p : presets)
            if (ImGui::Selectable(p.name)) {   // keeps its size where it can, centred on the anchor
                pushUndo();
                float w = u.offsetMax[0] - u.offsetMin[0], h = u.offsetMax[1] - u.offsetMin[1];
                bool fillX = p.a0x != p.a1x, fillY = p.a0y != p.a1y;
                u.anchorMin[0] = p.a0x, u.anchorMin[1] = p.a0y, u.anchorMax[0] = p.a1x, u.anchorMax[1] = p.a1y;
                auto side = [](float a, float size, float& lo, float& hi) {   // (inside the parent at the edges)
                    lo = a == 0.f ? 20.f : a == 1.f ? -20.f - size : -size * 0.5f;
                    hi = lo + size;
                };
                if (fillX) u.offsetMin[0] = u.offsetMax[0] = 0.f;
                else side(p.a0x, w, u.offsetMin[0], u.offsetMax[0]);
                if (fillY) u.offsetMin[1] = u.offsetMax[1] = 0.f;
                else side(p.a0y, h, u.offsetMin[1], u.offsetMax[1]);
            }
        ImGui::EndCombo();
    }
    tooltip("Where it sits in its parent element's rectangle (the screen, at the top): a point, or stretched");
    ImGui::DragFloat2("Anchor min", u.anchorMin, 0.01f, 0.f, 1.f, "%.2f");
    track();
    ImGui::DragFloat2("Anchor max", u.anchorMax, 0.01f, 0.f, 1.f, "%.2f");
    track();
    ImGui::DragFloat2("Offset min", u.offsetMin, 1.f, 0.f, 0.f, "%.0f");
    track();
    tooltip("Its left and top edges, from the anchor min point (canvas units: the screen is 1080 high)");
    ImGui::DragFloat2("Offset max", u.offsetMax, 1.f, 0.f, 0.f, "%.0f");
    track();
    ImGui::SeparatorText("Look");
    ImGui::ColorEdit3("Colour", &u.color.x);
    track();
    ImGui::SliderFloat("Opacity", &u.opacity, 0.f, 1.f, "%.2f");
    track();
    ImGui::DragFloat("Rounding", &u.rounding, 0.2f, 0.f, 100.f, "%.0f");
    track();
    if (u.kind == UIElement::Kind::Image) {
        ImGui::InputText("Image", &u.image);
        track();
        if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Texture, dropped)) {
            pushUndo();
            u.image = project::toStored(dropped.u8string());
        }
    }
    ImGui::InputText(u.kind == UIElement::Kind::TextField ? "Placeholder" : "Text", &u.text);
    track();
    tooltip("{@key}: the player's language (strings/<lang>.json); {name}: a game state value");
    ImGui::DragFloat("Text size", &u.textSize, 0.5f, 6.f, 400.f, "%.0f");
    track();
    ImGui::ColorEdit3("Text colour", &u.textColor.x);
    track();
    const char* aligns[] = {"Left", "Centre", "Right"};
    if (ImGui::Combo("Align", &u.align, aligns, 3)) editBegan_ = true;
    ImGui::InputText("Font", &u.font);
    track();
    if (focusable(u) || u.kind == UIElement::Kind::Slider) ImGui::SeparatorText("Behaviour");
    if (u.kind != UIElement::Kind::Button && focusable(u)) {
        ImGui::InputText("Value", &u.value);
        track();
        tooltip("The game state value it shows and changes (ctx.state(), game.state in Lua)");
    }
    if (u.kind == UIElement::Kind::Slider) {
        ImGui::DragFloat("Min", &u.min, 0.01f);
        track();
        ImGui::DragFloat("Max", &u.max, 0.01f);
        track();
    }
    if (focusable(u)) {
        ImGui::InputText("Action", &u.action);
        track();
        tooltip("Pressed (or changed): a message by this name, to the target entity (or everyone); or @resume, "
                "@pause, @quit, @load <scene>, @show / @hide / @toggle <entity>, @save / @loadgame <slot>");
        ImGui::InputText("Target", &u.target);
        track();
    }
    if (u.kind == UIElement::Kind::List) {
        ImGui::DragFloat("Spacing", &u.spacing, 0.5f, 0.f, 500.f, "%.0f");
        track();
        tooltip("Its children are stacked top to bottom, each as tall as its offsets say");
    }
}

// The scene's UI drawn over the viewport while editing (not playing). Clicks pick elements (the smallest one under
// the mouse; see-through full-screen canvases don't count); dragging the selected one moves it. True when it took
// the mouse.
bool Editor::drawGameUIPreview() {
    if (playing() || !showGameUi_ || viewingGameCamera()) return false;
    UILayout layout;
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        gameUi_.begin(dl, vx_, vy_, vw_, vh_, false);
        layout = layoutUI(scene_, gameUi_.width());
        if (!layout.order.empty()) {
            UIController::Frame f;
            f.interactive = false;
            // Texts in the project's language ({@key}); game state values ({score}) show as they're written
            if (previewStringsAt_ < 0.0 || ImGui::GetTime() - previewStringsAt_ > 2.0) {   // (edits picked up)
                previewStrings_.load(project::root(), project_.language);
                previewStringsAt_ = ImGui::GetTime();
            }
            f.format = [this](const std::string& t) { return previewStrings_.translate(t); };
            std::vector<UIAction> none;
            UIController preview;
            preview.frame(gameUi_, scene_, layout, f, none);
        }
        gameUi_.end();
    }
    if (layout.order.empty()) return false;
    float scale = vh_ / 1080.f;
    auto toScreen = [&](float x, float y) { return ImVec2(vx_ + x * scale, vy_ + y * scale); };
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (auto it = layout.rects.find(selected_); it != layout.rects.end()) {   // the selected one's outline
        const UIRect& r = it->second;
        dl->AddRect(toScreen(r.x, r.y), toScreen(r.x + r.w, r.y + r.h), IM_COL32(255, 160, 40, 255), 0.f, 0, 2.f);
    }
    ImGuiIO& io = ImGui::GetIO();
    if (uiDragging_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) uiDragging_ = false;
    if (uiDragging_) {
        if (Entity* e = selection(); e && e->ui) {
            float dx = io.MouseDelta.x / scale, dy = io.MouseDelta.y / scale;
            for (int k = 0; k < 2; ++k) {
                float d = k == 0 ? dx : dy;
                e->ui->offsetMin[k] += d;
                e->ui->offsetMax[k] += d;
            }
        }
        return true;
    }
    if (!viewportHovered() || nav_ != Nav::None || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;
    float mx = (io.MousePos.x - vx_) / scale, my = (io.MousePos.y - vy_) / scale, canvasW = gameUi_.width();
    EntityId best = kNoEntity;
    float bestArea = FLT_MAX;
    for (EntityId id : layout.order) {
        const UIRect& r = layout.rects.at(id);
        const Entity* e = scene_.find(id);
        if (!e || !r.contains(mx, my)) continue;
        bool backdrop = e->ui->opacity <= 0.f && e->ui->text.empty() && r.w * r.h > canvasW * 1080.f * 0.8f;
        if (backdrop) continue;
        if (r.w * r.h <= bestArea) bestArea = r.w * r.h, best = id;
    }
    if (best == kNoEntity) return false;
    if (best == selected_) {   // pressed on the selected one: drag it
        pushUndo();
        uiDragging_ = true;
    } else {
        select(best);
    }
    return true;
}
