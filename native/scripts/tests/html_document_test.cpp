#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "html/HTMLDocument.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace html;

namespace {

struct FakeFont : IHtmlFont {
    std::uint32_t style = 0;
    void Release() override {}
    void SetStyle(std::uint32_t s, bool enable) override { if (s == 0xffffffffu) style = 0; else if (enable) style |= s; }
    int LineHeight() override { return 10; }
    int GlyphHeight(int, int) override { return 9; }
    int Ascent() override { return 8; }
    int CharsThatFit(const char* text, int length, int maxWidth, int, int) override {
        const int fit = maxWidth / 8;
        if (fit >= length) return length;
        int last = -1;
        for (int i = 0; i < fit + 1 && i < length; ++i) if (text[i] == ' ') last = i;
        return last >= 0 ? last + 1 : 0;
    }
    int FirstWordLength(const char* text, int length, int, int* wordWidth) override {
        int n = 0;
        while (n < length && text[n] != ' ') ++n;
        if (n < length) ++n;
        *wordWidth = n * 8;
        return n;
    }
    int StringWidth(const char*, int length) override { return length * 8; }
    void DrawText(IHtmlSurface*, int, int, const char*, int, Color, std::uint32_t, const cRZRect*) override {}
};

struct FakeFonts : IHtmlFontSystem {
    std::vector<FakeFont*> made;
    std::vector<std::string> requests;
    IHtmlFont* CreateFont(const std::string& face, std::uint32_t kind, std::uint32_t size, std::uint32_t style, bool) override {
        requests.push_back(face + "/" + std::to_string(kind) + "/" + std::to_string(size) + "/" + std::to_string(style));
        made.push_back(new FakeFont());
        return made.back();
    }
    ~FakeFonts() { for (auto* f : made) delete f; }
};

struct FakeGraphics : IHtmlGraphics {
    Color MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return 0xff000000u | (r << 16) | (g << 8) | b; }
};

struct Bmp : IHtmlBitmap {
    int refs = 0;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    int Width() override { return 20; }
    int Height() override { return 10; }
    cRZRect Bounds() override { return cRZRect{0, 0, 20, 10}; }
};

struct FakeWeb : IHtmlWebSystem {
    std::vector<std::string> started;
    std::vector<std::uint32_t> cancelled;
    std::uint32_t StartJob(const std::string& url) override { started.push_back(url); return 100 + static_cast<std::uint32_t>(started.size()); }
    // the facade's reporting overload: who each job reports back to
    std::vector<std::pair<std::string, IHtmlDownloadTarget*>> targets;
    std::uint32_t StartJob(const std::string& url, IHtmlDownloadTarget* target) override {
        targets.push_back({url, target});
        return StartJob(url);
    }
    void CancelJob(std::uint32_t id) override { cancelled.push_back(id); }
    bool IsVisited(const std::string& url) override { return url == "http://x/visited.html"; }
};

struct FakeImages : IHtmlImageDecoder {
    Bmp bmp;
    int loads = 0;
    bool LoadImageFile(const std::string&, IHtmlBitmap*& bitmap) override { ++loads; bitmap = &bmp; return true; }
};

struct FakeWindow : IHtmlWindow {
    cHTMLBODY* body = nullptr;
    int invalidated = 0;
    cHTMLBODY* Body() override { return body; }
    Color DefaultActiveLinkColor() override { return 1; }
    Color DefaultVisitedLinkColor() override { return 2; }
    void Invalidate() override { ++invalidated; }
};

struct Setup {
    FakeFonts fonts;
    FakeGraphics gfx;
    FakeWeb web;
    FakeImages images;
    FakeWindow window;
    HtmlServices services;
    Setup() {
        services.graphics = &gfx;
        services.fonts = &fonts;
        services.images = &images;
        services.resolveUrl = [](const std::string& href, const std::string& base) {
            if (href.find("://") != std::string::npos) return href;
            return base.substr(0, base.rfind('/') + 1) + href;
        };
        services.makeTempPath = [] { return std::string("/tmp/html_doc_test_tmp.bin"); };
        services.createBullet = [](int, Color, Color, bool) { static Bmp bullet; return static_cast<IHtmlBitmap*>(&bullet); };
    }
};

int Count(const cHTMLContainerElement& c, int type) {
    int n = 0;
    for (cHTMLElement* child : c.children()) {
        if (child->type() == type) ++n;
        if (child->IsContainer()) n += Count(static_cast<cHTMLContainerElement&>(*child), type);
    }
    return n;
}

}  // namespace

int main() {
    // ---- tag and entity tables
    CHECK(LookupTag("P") == kTagP && LookupTag("/P") == kTagEndP && LookupTag("H3") == kTagH3 && LookupTag("BOGUS") == 0);
    CHECK(LookupTag("OPTGROUP") == kTagOptgroup && LookupTag("COLGROUP") == kTagColgroup && LookupTag("!DOCTYPE") == kTokenDoctype);
    const tFirstSets& sets = FirstSets();
    CHECK(sets.fontStyle.size() == 8 && sets.phrase.size() == 10 && sets.special.size() == 12 && sets.formCtrl.size() == 5);
    CHECK(sets.inlineSet.count(kTokenText) && sets.inlineSet.count(kTagB) && !sets.inlineSet.count(kTagP));
    CHECK(sets.block.count(kTagH2) && sets.block.count(kTagUl) && sets.flow.count(kTagA) && sets.flow.count(kTagTable));

    Setup setup;
    {   // ---- a small document
        cHTMLDocument doc(setup.services);
        doc.Init(&setup.window, &setup.web, cRZString("http://x/dir/page.html"));
        std::string html =
            "<!-- leading comment -->\n<HTML><HEAD><TITLE>My Page</TITLE><META NAME=\"author\" CONTENT='me'>"
            "<LINK HREF=\"style.css\" REV=x><BASE HREF=\"http://x/other/\"></HEAD>\n"
            "<BODY BGCOLOR=\"#102030\" LINK=blue TEXT=red>\n"
            "<H1>Title</H1><P>Hello <B>bold &amp; <I>italic</I></B> <A HREF=\"a.html\" NAME=top>link</A> "
            "<A HREF=\"http://x/visited.html\">seen</A>"
            "<HR WIDTH=\"50%\" SIZE=3><IMG SRC=\"pic.gif\" WIDTH=20 HEIGHT=10 ALT=\"a picture\">"
            "<UL><LI>one<LI>two</UL><OL><LI>first</OL>"
            "<TABLE BORDER=1><TR><TD>a<TD ALIGN=right>b</TR><TR><TD COLSPAN=2>c</TR></TABLE>"
            "<FONT SIZE=+2 COLOR=\"#00ff00\" FACE=\"Times\">big</FONT><CENTER>mid</CENTER><PRE>x  y</PRE>\n"
            "</BODY></HTML>";
        CHECK(doc.ParseSGMLElements(cRZString(html.c_str())));
        CHECK(doc.title() == "My Page");
        CHECK(doc.metas().size() == 1 && doc.metas()[0]->name->str() == "author" && doc.metas()[0]->content->str() == "me");
        CHECK(doc.links().size() == 1 && !doc.links()[0]->flag && doc.links()[0]->href->str() == "style.css");
        cRZString base;
        CHECK(doc.GetBaseURL(base) && base.str() == "http://x/other/");
        cHTMLBODY* body = doc.Body();
        CHECK(body && body->hasBackgroundColor && body->backgroundColor == 0xff102030);
        CHECK(Count(*body, 0x13) == 1);             // CENTER builds a DIV
        CHECK(Count(*body, kTypeAnchor) == 2);
        CHECK(Count(*body, kTypeImg) >= 2);          // the picture and the bullets
        CHECK(Count(*body, kTypeTable) == 1);
        CHECK(Count(*body, kTypeTD) == 3);
        CHECK(Count(*body, 5) == 1);                 // HR
        CHECK(doc.FindAnchor(cRZString("top")) != nullptr && doc.FindAnchor(cRZString("nope")) == nullptr);
        // images were requested through the web system relative to the (new) base URL
        CHECK(!setup.web.started.empty());
        bool requestedPicture = false;
        for (auto& url : setup.web.started) requestedPicture |= url == "http://x/other/pic.gif";
        CHECK(requestedPicture);
        // fonts: B -> bold flag, H1 -> 40px, FONT SIZE=+2 -> table[5], FACE -> kind 0
        bool sawHeading = false, sawTimes = false;
        for (auto& r : setup.fonts.requests) {
            sawHeading |= r.find("arial/0/40/1") != std::string::npos;
            sawTimes |= r.rfind("Times/0/24/", 0) == 0;
        }
        CHECK(sawHeading && sawTimes);
        // the visited link takes the visited colour, the other the body's LINK colour
        std::vector<cHTMLAnchor*> anchors;
        for (cHTMLElement* e : body->children())
            if (e->IsContainer()) for (cHTMLElement* c : static_cast<cHTMLContainerElement*>(e)->children())
                if (c->type() == kTypeAnchor) anchors.push_back(static_cast<cHTMLAnchor*>(c));
        CHECK(anchors.size() == 2 && anchors[0]->href()->str() == "a.html" && anchors[0]->hrefAbsolute()->str() == "http://x/other/a.html");

        // image delivery: first download resolves the pending requesters and the page needs a relayout
        tDownloadedJob job;
        job.url = "http://x/other/pic.gif";
        job.isFile = true;
        job.data = "/tmp/whatever.gif";
        // the document asked to be told itself; the facade's delivery marks the layout stale and repaints the window
        IHtmlDownloadTarget* target = nullptr;
        for (auto& t : setup.web.targets) if (t.first == job.url) target = t.second;
        CHECK(target == static_cast<IHtmlDownloadTarget*>(&doc));
        doc.MarkPageLayoutStatus(true);
        const int paintsBefore = setup.window.invalidated;
        if (target) target->Downloaded(true, job);
        CHECK(setup.images.loads == 1);
        CHECK(!doc.IsPageLayoutDone());
        CHECK(setup.window.invalidated == paintsBefore + 1);
        if (target) target->Downloaded(false, job);   // a failed job changes nothing
        CHECK(setup.images.loads == 1);
        cHTMLIMG* firstImg = nullptr;
        std::function<void(cHTMLContainerElement&)> find = [&](cHTMLContainerElement& c) {
            for (cHTMLElement* child : c.children()) {
                if (child->type() == kTypeImg && static_cast<cHTMLIMG*>(child)->source && !firstImg) firstImg = static_cast<cHTMLIMG*>(child);
                if (child->IsContainer()) find(static_cast<cHTMLContainerElement&>(*child));
            }
        };
        find(*body);
        CHECK(firstImg && firstImg->bitmap == &setup.images.bmp && firstImg->width == 20);
        CHECK(!doc.DoMessage(0x1234, nullptr));

        // layout of the whole page
        struct Surface : IHtmlSurface {
            bool Begin(std::uint32_t) override { return true; }
            void End(std::uint32_t) override {}
            void FillRect(Color, const cRZRect&, const cRZRect*) override {}
            void DrawLine(int, int, int, int, Color, int) override {}
            void Blit(IHtmlBitmap*, const cRZRect&, const cRZRect&, const cRZRect*) override {}
            void BlitStretched(IHtmlBitmap*, const cRZRect&, const cRZRect&, const cRZRect*) override {}
        } surface;
        cHTMLDrawContext ctx;
        ctx.graphics = &setup.gfx;
        ctx.surface = &surface;
        ctx.window = &setup.window;
        ctx.screenRect = cRZRect{0, 0, 300, 2000};
        ctx.documentRect = cRZRect{0, 0, 300, 2000};
        body->Layout(ctx);
        body->PostLayout();
        CHECK(body->laidOut() && ctx.drawY > 60);          // content stacked well below the heading
                // (a table inherits the cursor column of the preceding list: it starts at x = 44 and spans the 300 available pixels)
        CHECK(body->drawRect().bottom > 60 && body->drawRect().right == 344);
        body->Draw(ctx);
        // clicking a link recolours its text and repaints (one repaint more than the picture's delivery above)
        anchors[0]->OnMouseDownL(ctx);
        CHECK(setup.window.invalidated == 2);
    }
    std::printf(failures ? "FAILED\n" : "html_document: all passed\n");
    return failures ? 1 : 0;
}
