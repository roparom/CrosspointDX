#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

namespace MilitaryMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 30,
                                 .headerHeight = 50,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 32,
                                 .listWithSubtitleRowHeight = 58,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 12,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 30,
                                 .keyboardKeySpacing = 10,
                                 .keyboardBottomAligned = false,
                                 .keyboardCenteredText = false};
}



class MilitaryTheme : public BaseTheme {
 public:
  virtual ~MilitaryTheme() = default;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true) const;  // Right aligned (UI headers)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                               const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                               bool inactiveSelection = false) const;
  virtual void drawSpinner(const GfxRenderer& renderer, int centerX, int centerY, const char* text,
                           int frame) const;
  virtual bool showsFileIcons() const { return false; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};