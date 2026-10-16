#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Dark studio look: pink accent, weathered rust edges. All rust textures are
// generated once in the constructor and cached; paint code only samples them.
namespace palette {
inline const juce::Colour background { 0xff121014 };
inline const juce::Colour panel      { 0xff1b171d };
inline const juce::Colour well       { 0xff0d0b0e }; // recessed fields, tiles
inline const juce::Colour text       { 0xfff3e9ef };
inline const juce::Colour dimText    { 0xffa8949f };
inline const juce::Colour pink       { 0xffff4fa8 };
inline const juce::Colour pinkDeep   { 0xff9c2f68 };
inline const juce::Colour warn       { 0xffffb02e };
inline const juce::Colour fault      { 0xffff4b3e };
inline const juce::Colour rustDark   { 0xff3b1d10 };
inline const juce::Colour rust       { 0xff8a3f17 };
inline const juce::Colour rustLight  { 0xffd9822b };
}

class RustLookAndFeel : public juce::LookAndFeel_V4 {
public:
    RustLookAndFeel();
    // A rusted frame stroked along a rounded rectangle. `thickness` is in pixels.
    void drawRustFrame(juce::Graphics&,juce::Rectangle<float> bounds,float corner,float thickness) const;
    void drawRivet(juce::Graphics&,juce::Point<float> centre,float radius) const;
    juce::FillType rustFill(float scale=1.0f) const;
    static juce::Font font(float height,bool bold=false);

    void drawButtonBackground(juce::Graphics&,juce::Button&,const juce::Colour& background,bool highlighted,bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&,int buttonHeight) override;
    void drawRotarySlider(juce::Graphics&,int x,int y,int width,int height,float position,float startAngle,float endAngle,juce::Slider&) override;
    void drawComboBox(juce::Graphics&,int width,int height,bool down,int buttonX,int buttonY,int buttonW,int buttonH,juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void fillTextEditorBackground(juce::Graphics&,int width,int height,juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&,int width,int height,juce::TextEditor&) override;
private:
    juce::Image rustTexture; // tileable 256x256 corrosion
};
