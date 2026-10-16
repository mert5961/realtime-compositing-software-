#include "RustLookAndFeel.h"
#include <cmath>
#include <cstdint>

using namespace juce;

namespace {
float hash(int x,int y,int seed) {
    uint32_t h=uint32_t(x)*374761393u+uint32_t(y)*668265263u+uint32_t(seed)*2246822519u;
    h=(h^(h>>13))*1274126177u; h^=h>>16;
    return float(h&0xffffffu)/float(0xffffffu);
}
// Tileable value noise: lattice coordinates wrap at `period`.
float valueNoise(float x,float y,int period,int seed) {
    const int xi=int(std::floor(x)),yi=int(std::floor(y));
    const float fx=x-float(xi),fy=y-float(yi);
    const auto at=[&](int i,int j) { return hash((i%period+period)%period,(j%period+period)%period,seed); };
    const float sx=fx*fx*(3-2*fx),sy=fy*fy*(3-2*fy);
    const float top=at(xi,yi)+(at(xi+1,yi)-at(xi,yi))*sx, bottom=at(xi,yi+1)+(at(xi+1,yi+1)-at(xi,yi+1))*sx;
    return top+(bottom-top)*sy;
}
float fbm(float u,float v,int baseFrequency,int octaves,int seed) {
    float sum=0,amplitude=.5f,norm=0;
    for(int o=0;o<octaves;++o) {
        const int frequency=baseFrequency<<o;
        sum+=amplitude*valueNoise(u*float(frequency),v*float(frequency),frequency,seed+o*17); norm+=amplitude; amplitude*=.5f;
    }
    return sum/norm;
}
Colour rustRamp(float t) {
    // dark scale -> oxide -> orange flake -> pale ochre highlights
    const Colour ochre(0xffe0a35a);
    if(t<.45f) return palette::rustDark.interpolatedWith(palette::rust,t/.45f);
    if(t<.8f) return palette::rust.interpolatedWith(palette::rustLight,(t-.45f)/.35f);
    return palette::rustLight.interpolatedWith(ochre,(t-.8f)/.2f);
}
Image makeRustTexture(int size) {
    Image image(Image::ARGB,size,size,true,SoftwareImageType());
    {
        Image::BitmapData pixels(image,Image::BitmapData::writeOnly);
        for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
            const float u=float(x)/float(size),v=float(y)/float(size);
            const float base=fbm(u,v,4,5,11);
            const float streak=fbm(u*.5f,v,8,3,29);              // vertical run-off streaks
            const float pit=fbm(u,v,16,3,47);                     // corrosion pits
            float t=jlimit(0.f,1.f,(base-.3f)*1.7f+(streak-.5f)*.35f);
            Colour c=rustRamp(t);
            if(pit>.66f) c=c.interpolatedWith(Colour(0xff1a0c06),jlimit(0.f,1.f,(pit-.66f)*6));
            if(hash(x,y,83)>.985f) c=c.brighter(.35f);            // flakes catching light
            const float alpha=jlimit(.55f,1.f,.7f+base*.45f);     // eroded, uneven coverage
            pixels.setPixelColour(x,y,c.withAlpha(alpha));
        }
    }
    return NativeImageType().convert(image);
}
}

RustLookAndFeel::RustLookAndFeel():rustTexture(makeRustTexture(256)) {
    setColourScheme({ palette::background, palette::panel, palette::panel, palette::rust, palette::text,
                      palette::pinkDeep, palette::text, palette::pink, palette::text });
    setColour(ResizableWindow::backgroundColourId,palette::background);
    setColour(Label::textColourId,palette::text);
    setColour(TextButton::buttonColourId,palette::panel);
    setColour(TextButton::textColourOffId,palette::text);
    setColour(TextButton::textColourOnId,palette::text);
    setColour(TextEditor::backgroundColourId,palette::well);
    setColour(TextEditor::textColourId,palette::text);
    setColour(TextEditor::highlightColourId,palette::pinkDeep);
    setColour(TextEditor::outlineColourId,Colours::transparentBlack);
    setColour(TextEditor::focusedOutlineColourId,palette::pink);
    setColour(CaretComponent::caretColourId,palette::pink);
    setColour(ComboBox::backgroundColourId,palette::well);
    setColour(ComboBox::textColourId,palette::text);
    setColour(ComboBox::arrowColourId,palette::pink);
    setColour(PopupMenu::backgroundColourId,palette::panel);
    setColour(PopupMenu::textColourId,palette::text);
    setColour(PopupMenu::highlightedBackgroundColourId,palette::pinkDeep);
    setColour(PopupMenu::highlightedTextColourId,palette::text);
    setColour(Slider::textBoxTextColourId,palette::text);
    setColour(Slider::textBoxBackgroundColourId,palette::well);
    setColour(Slider::textBoxOutlineColourId,Colours::transparentBlack);
    setColour(Slider::textBoxHighlightColourId,palette::pinkDeep);
    setColour(ListBox::backgroundColourId,palette::well);
    setColour(ScrollBar::thumbColourId,palette::pinkDeep);
}

Font RustLookAndFeel::font(float height,bool bold) { return Font(FontOptions(height,bold ? Font::bold:Font::plain)); }

FillType RustLookAndFeel::rustFill(float scale) const {
    return FillType(rustTexture,AffineTransform::scale(scale));
}

void RustLookAndFeel::drawRustFrame(Graphics& g,Rectangle<float> bounds,float corner,float thickness) const {
    const auto centre=bounds.reduced(thickness*.5f);
    Path outline; outline.addRoundedRectangle(centre,corner);
    Path band; PathStrokeType(thickness).createStrokedPath(band,outline);
    g.setColour(Colours::black.withAlpha(.55f)); g.fillPath(band,AffineTransform::translation(0,1.5f));
    // Anchor the texture to the frame position so neighbouring frames corrode differently.
    g.setFillType(FillType(rustTexture,AffineTransform::translation(-bounds.getX()*1.7f,-bounds.getY()*.6f)));
    g.fillPath(band);
    Path inner; inner.addRoundedRectangle(bounds.reduced(thickness),jmax(0.f,corner-thickness*.5f));
    g.setColour(palette::rustLight.withAlpha(.28f)); g.strokePath(inner,PathStrokeType(1));
    Path outer; outer.addRoundedRectangle(bounds.reduced(.5f),corner+thickness*.5f);
    g.setColour(Colour(0xff140805).withAlpha(.9f)); g.strokePath(outer,PathStrokeType(1));
}

void RustLookAndFeel::drawRivet(Graphics& g,Point<float> centre,float radius) const {
    const auto area=Rectangle<float>(radius*2,radius*2).withCentre(centre);
    g.setGradientFill(ColourGradient(palette::rustLight,centre.translated(-radius*.4f,-radius*.4f),palette::rustDark,centre.translated(radius,radius),true));
    g.fillEllipse(area);
    g.setColour(Colours::black.withAlpha(.6f)); g.drawEllipse(area,1);
}

void RustLookAndFeel::drawButtonBackground(Graphics& g,Button& button,const Colour& background,bool highlighted,bool down) {
    auto bounds=button.getLocalBounds().toFloat().reduced(1);
    auto fill=background;
    if(!button.isEnabled()) fill=fill.withMultipliedSaturation(.3f).withMultipliedBrightness(.45f);
    else if(down) fill=fill.darker(.25f);
    else if(highlighted) fill=fill.brighter(.12f);
    g.setColour(fill); g.fillRoundedRectangle(bounds.reduced(2),4);
    drawRustFrame(g,bounds,5,3);
}

Font RustLookAndFeel::getTextButtonFont(TextButton&,int buttonHeight) { return font(jmin(17.f,float(buttonHeight)*.5f),true); }

void RustLookAndFeel::drawRotarySlider(Graphics& g,int x,int y,int width,int height,float position,float startAngle,float endAngle,Slider& slider) {
    const auto bounds=Rectangle<int>(x,y,width,height).toFloat().reduced(4);
    const float radius=jmin(bounds.getWidth(),bounds.getHeight())*.5f;
    const auto centre=bounds.getCentre();
    const float bezel=jmax(4.f,radius*.16f);
    const auto disc=Rectangle<float>(radius*2,radius*2).withCentre(centre);
    // Rust bezel ring.
    Path ring; ring.addEllipse(disc.reduced(bezel*.5f));
    Path band; PathStrokeType(bezel).createStrokedPath(band,ring);
    g.setFillType(FillType(rustTexture,AffineTransform::translation(-disc.getX()*2.3f,-disc.getY()*1.1f))); g.fillPath(band);
    g.setColour(Colours::black.withAlpha(.7f)); g.drawEllipse(disc.reduced(.5f),1);
    // Knob body.
    const auto body=disc.reduced(bezel*1.05f);
    g.setGradientFill(ColourGradient(Colour(0xff2b252d),body.getTopLeft(),palette::well,body.getBottomRight(),false));
    g.fillEllipse(body);
    // Value track and arc.
    const float arcRadius=body.getWidth()*.5f-bezel*.9f, arcWidth=jmax(3.f,radius*.1f);
    const float angle=startAngle+position*(endAngle-startAngle);
    Path track; track.addCentredArc(centre.x,centre.y,arcRadius,arcRadius,0,startAngle,endAngle,true);
    g.setColour(palette::pinkDeep.withAlpha(.35f)); g.strokePath(track,PathStrokeType(arcWidth,PathStrokeType::curved,PathStrokeType::rounded));
    const auto accent=slider.isEnabled() ? palette::pink:palette::dimText;
    Path value; value.addCentredArc(centre.x,centre.y,arcRadius,arcRadius,0,startAngle,angle,true);
    g.setColour(accent); g.strokePath(value,PathStrokeType(arcWidth,PathStrokeType::curved,PathStrokeType::rounded));
    // Pointer.
    const auto tip=centre.getPointOnCircumference(arcRadius-arcWidth*1.5f,angle);
    const auto root=centre.getPointOnCircumference(arcRadius*.25f,angle);
    g.setColour(palette::text); g.drawLine({root,tip},jmax(2.f,radius*.06f));
    drawRivet(g,centre,jmax(3.f,radius*.12f));
}

void RustLookAndFeel::drawComboBox(Graphics& g,int width,int height,bool,int buttonX,int buttonY,int buttonW,int buttonH,ComboBox& box) {
    const auto bounds=Rectangle<int>(0,0,width,height).toFloat();
    g.setColour(box.findColour(ComboBox::backgroundColourId)); g.fillRoundedRectangle(bounds.reduced(2),3);
    drawRustFrame(g,bounds,4,2.5f);
    const auto arrowZone=Rectangle<int>(buttonX,buttonY,buttonW,buttonH).toFloat().reduced(buttonW*.3f,buttonH*.38f);
    Path arrow; arrow.addTriangle(arrowZone.getTopLeft(),arrowZone.getTopRight(),{arrowZone.getCentreX(),arrowZone.getBottom()});
    g.setColour(box.findColour(ComboBox::arrowColourId).withAlpha(box.isEnabled() ? 1.f:.3f)); g.fillPath(arrow);
}

Font RustLookAndFeel::getComboBoxFont(ComboBox&) { return font(15); }
Font RustLookAndFeel::getLabelFont(Label& label) { return label.getFont(); }
Font RustLookAndFeel::getPopupMenuFont() { return font(15); }

void RustLookAndFeel::fillTextEditorBackground(Graphics& g,int width,int height,TextEditor& editor) {
    g.setColour(editor.findColour(TextEditor::backgroundColourId)); g.fillRoundedRectangle(Rectangle<int>(width,height).toFloat().reduced(2),3);
}

void RustLookAndFeel::drawTextEditorOutline(Graphics& g,int width,int height,TextEditor& editor) {
    const auto bounds=Rectangle<int>(width,height).toFloat();
    drawRustFrame(g,bounds,4,2.5f);
    if(editor.hasKeyboardFocus(true) && !editor.isReadOnly()) { g.setColour(palette::pink); g.drawRoundedRectangle(bounds.reduced(3),3,1.5f); }
}
