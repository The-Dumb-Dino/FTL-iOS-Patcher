// The window (wxWidgets: the system's own controls): unpack, mods with ftlman (optional), pack.
#include <wx/wx.h>
#include <wx/clipbrd.h>
#include <wx/checkbox.h>
#include <wx/hyperlink.h>

#include <atomic>
#include <functional>
#include <thread>

#include "patch.h"

using namespace ftl;

static const char *FTLMAN_URL = "https://github.com/afishhh/ftlman/releases";

static wxString Wx(const std::string &s)
{
    return wxString::FromUTF8(s);
}
static std::string Utf8(const wxString &s)
{
    return std::string(s.utf8_str());
}

namespace
{

class Frame : public wxFrame
{
public:
    Frame();

private:
    wxPanel *panel;
    wxTextCtrl *ipa, *name, *bundle, *icon, *hsDylib, *out, *log;
    wxButton *chooseIpa, *unpackBtn, *openBtn, *copyBtn, *otherBtn, *chooseIcon, *chooseHs, *chooseOut, *packBtn;
    wxStaticText *folderText, *hsText;
    wxCheckBox *advanced;
    std::vector<wxWindow *> advancedWidgets;  // greyed out until "Advanced" is ticked
    wxGauge *gauge;
    wxTimer pulse, poll;

    fs::path folder;  // the unpacked folder
    std::atomic<bool> busy{false};

    // A section: a bold heading above an untitled box (box titles are small on macOS)
    wxStaticBoxSizer *Section(wxSizer *top, const wxString &title, int gap)
    {
        auto *h = new wxStaticText(panel, wxID_ANY, title);
        h->SetFont(h->GetFont().Scaled(1.15f).Bold());
        top->Add(h, 0, wxLEFT | wxRIGHT | wxTOP, gap * 2);
        top->AddSpacer(gap);
        auto *box = new wxStaticBoxSizer(wxVERTICAL, panel, "");
        top->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT, gap * 2);
        return box;
    }

    wxStaticText *Note(wxWindow *parent, const wxString &text, int wrap = 0)
    {
        auto *t = new wxStaticText(parent, wxID_ANY, text);
        if (wrap) t->Wrap(FromDIP(wrap));
        return t;
    }

    // Lines from the worker thread
    void LogLine(const std::string &s)
    {
        CallAfter([this, s] { log->AppendText(Wx(s) + "\n"); });
    }

    void SetFolder(const fs::path &dir);
    void RefreshHyperspace();
    void UpdateState();
    void Run(std::function<void()> job);

    void OnChooseIpa(wxCommandEvent &);
    void OnUnpack(wxCommandEvent &);
    void OnOther(wxCommandEvent &);
    void OnPack(wxCommandEvent &);
};

Frame::Frame() : wxFrame(nullptr, wxID_ANY, "FTL iOS Patcher"), pulse(this), poll(this)
{
    panel = new wxPanel(this);
    auto *top = new wxBoxSizer(wxVERTICAL);
    const int gap = FromDIP(8), wrap = 600;

    auto *title = new wxStaticText(panel, wxID_ANY, "FTL iOS Patcher");
    title->SetFont(title->GetFont().Scaled(1.6f).Bold());
    top->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, gap * 2);
    top->Add(Note(panel,
                  "Patches the FTL: Faster Than Light IPA file to allow running on an iPad, iPhone, iPhone Duo "
                  "with various QOL settings, mods and FTL: Hyperspace.",
                  wrap),
             0, wxALL, gap * 2);

    // 1. Unpack
    auto *box1 = Section(top, "1.  Unpack the Game", gap);
    wxWindow *b1 = box1->GetStaticBox();
    box1->Add(Note(b1, "A decrypted FTL 1.6.21 IPA"), 0, wxALL, gap);
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    ipa = new wxTextCtrl(b1, wxID_ANY);
    ipa->SetHint("FTL.ipa");
    chooseIpa = new wxButton(b1, wxID_ANY, "Choose...");
    unpackBtn = new wxButton(b1, wxID_ANY, "Unpack");
    row->Add(ipa, 1, wxALIGN_CENTER_VERTICAL);
    row->Add(chooseIpa, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, gap);
    row->Add(unpackBtn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, gap);
    box1->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);
    row = new wxBoxSizer(wxHORIZONTAL);
    folderText =
        new wxStaticText(b1, wxID_ANY, "Not unpacked yet.", wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    openBtn = new wxButton(b1, wxID_ANY, "Open Folder");
    copyBtn = new wxButton(b1, wxID_ANY, "Copy Path");
    otherBtn = new wxButton(b1, wxID_ANY, "Other Folder...");
    otherBtn->SetToolTip("Use a folder unpacked before");
    folderText->SetMinSize(wxSize(FromDIP(60), -1));  // shortened with "..." when the window is narrow
    row->Add(folderText, 1, wxALIGN_CENTER_VERTICAL);
    for (auto *b : {openBtn, copyBtn, otherBtn}) row->Add(b, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, gap);
    box1->Add(row, 0, wxEXPAND | wxALL, gap);

    // 2. Mods
    auto *box2 = Section(top, "2.  FTL Modding", gap);
    wxWindow *b2 = box2->GetStaticBox();
    box2->Add(Note(b2,
                   "Skip this for the plain game. For mods: in ftlman's settings, set the FTL folder to the "
                   "unpacked folder above, choose your mods (Hyperspace.ftl first, for mods like Multiverse) "
                   "and press Apply. Hyperspace is included when the mods have it.",
                   wrap - 30),
              0, wxALL, gap);
    row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxHyperlinkCtrl(b2, wxID_ANY, "Get ftlman", FTLMAN_URL), 0, wxALIGN_CENTER_VERTICAL);
    hsText = new wxStaticText(b2, wxID_ANY, "");
    row->AddStretchSpacer();
    row->Add(hsText, 0, wxALIGN_CENTER_VERTICAL);
    box2->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    // 3. Pack
    auto *box3 = Section(top, "3.  Pack IPA", gap);
    wxWindow *b3 = box3->GetStaticBox();
    auto *grid = new wxFlexGridSizer(2, gap, gap);
    grid->AddGrowableCol(1);
    auto label = [&](const char *t)
    {
        auto *l = new wxStaticText(b3, wxID_ANY, t);
        grid->Add(l, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
        return l;
    };
    auto withButton = [&](wxTextCtrl *&field, wxButton *&btn, const char *hint)
    {
        auto *r = new wxBoxSizer(wxHORIZONTAL);
        field = new wxTextCtrl(b3, wxID_ANY);
        if (hint) field->SetHint(hint);
        btn = new wxButton(b3, wxID_ANY, "Choose...");
        r->Add(field, 1, wxALIGN_CENTER_VERTICAL);
        r->Add(btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, gap);
        grid->Add(r, 1, wxEXPAND);
    };
    label("Save as:");
    withButton(out, chooseOut, nullptr);
    label("Hyperspace:");
    withButton(hsDylib, chooseHs, "Needed when the mods include it: Hyperspace for iOS (.dylib)");
    hsDylib->SetToolTip("Hyperspace's iOS build, like Hyperspace.1.6.21.arm64.dylib. Its prepatch-hyperspace.ftlt "
                        "is taken from the same folder if it's there; without one, it must have the same hooks as "
                        "the patcher's prepatch.");
    grid->AddSpacer(0);
    advanced = new wxCheckBox(b3, wxID_ANY, "Advanced");
    grid->Add(advanced, 0, wxTOP, gap / 2);
    advancedWidgets.push_back(label("App name:"));
    name = new wxTextCtrl(b3, wxID_ANY);
    name->SetHint("Optional, FTL if empty");
    name->SetToolTip("The name on the home screen");
    grid->Add(name, 1, wxEXPAND);
    advancedWidgets.push_back(label("Bundle ID:"));
    bundle = new wxTextCtrl(b3, wxID_ANY);
    bundle->SetHint("Optional, like com.yourname.ftl-multiverse");
    bundle->SetToolTip("An ID of its own installs it next to another patched FTL. Each ID has its own saves.");
    grid->Add(bundle, 1, wxEXPAND);
    advancedWidgets.push_back(label("Icon:"));
    withButton(icon, chooseIcon, "Optional: a square PNG (1024x1024 is best) or a macOS .icns");
    for (wxWindow *w : std::initializer_list<wxWindow *>{name, bundle, icon, chooseIcon}) advancedWidgets.push_back(w);
    box3->Add(grid, 0, wxEXPAND | wxALL, gap);
    row = new wxBoxSizer(wxHORIZONTAL);
    gauge = new wxGauge(b3, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(160), -1));
    gauge->Hide();
    packBtn = new wxButton(b3, wxID_ANY, "Pack IPA");
    packBtn->SetDefault();
    row->Add(gauge, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer();
    row->Add(packBtn, 0, wxALIGN_CENTER_VERTICAL);
    box3->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    log = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(-1, FromDIP(150)),
                         wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
#if defined(__WXOSX__)
    const char *mono = "Menlo";
#elif defined(__WXMSW__)
    const char *mono = "Consolas";
#else
    const char *mono = "Monospace";
#endif
    log->SetFont(wxFont(wxFontInfo(log->GetFont().GetPointSize() - 1).Family(wxFONTFAMILY_TELETYPE).FaceName(mono)));
    top->Add(log, 1, wxEXPAND | wxALL, gap * 2);

    panel->SetSizer(top);
    auto *frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(panel, 1, wxEXPAND);
    SetSizerAndFit(frameSizer);
    SetMinSize(GetSize());

    chooseIpa->Bind(wxEVT_BUTTON, &Frame::OnChooseIpa, this);
    unpackBtn->Bind(wxEVT_BUTTON, &Frame::OnUnpack, this);
    otherBtn->Bind(wxEVT_BUTTON, &Frame::OnOther, this);
    advanced->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { UpdateState(); });
    packBtn->Bind(wxEVT_BUTTON, &Frame::OnPack, this);
    openBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxLaunchDefaultApplication(Wx(U8Str(folder))); });
    copyBtn->Bind(wxEVT_BUTTON,
                  [this](wxCommandEvent &)
                  {
                      if (wxTheClipboard->Open())
                      {
                          wxTheClipboard->SetData(new wxTextDataObject(Wx(U8Str(folder))));
                          wxTheClipboard->Close();
                          LogLine("Copied the folder's path: " + U8Str(folder));
                      }
                  });
    chooseIcon->Bind(wxEVT_BUTTON,
                     [this](wxCommandEvent &)
                     {
                         wxFileDialog d(this, "Choose an icon", "", "", "Images (*.png;*.icns)|*.png;*.icns",
                                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
                         if (d.ShowModal() == wxID_OK) icon->SetValue(d.GetPath());
                     });
    chooseHs->Bind(wxEVT_BUTTON,
                   [this](wxCommandEvent &)
                   {
                       wxFileDialog d(this, "Choose a Hyperspace build for iOS", "", "", "Hyperspace (*.dylib)|*.dylib",
                                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
                       if (d.ShowModal() == wxID_OK) hsDylib->SetValue(d.GetPath());
                   });
    chooseOut->Bind(wxEVT_BUTTON,
                    [this](wxCommandEvent &)
                    {
                        fs::path cur = U8Path(Utf8(out->GetValue()));
                        wxFileDialog d(this, "Save the IPA as", Wx(U8Str(cur.parent_path())), Wx(U8Str(cur.filename())),
                                       "IPA (*.ipa)|*.ipa", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
                        if (d.ShowModal() == wxID_OK) out->SetValue(d.GetPath());
                    });
    Bind(wxEVT_TIMER,
         [this](wxTimerEvent &e)
         {
             if (e.GetTimer().GetId() == pulse.GetId())
                 gauge->Pulse();
             else if (!busy)
                 RefreshHyperspace();  // notices mods applied in ftlman meanwhile
         });
    poll.Start(2000);

    fs::path files = FindFiles();
    if (!fs::is_regular_file(files / "ftlios.dylib"))
        LogLine("Warning: the patcher's files aren't next to it (" + U8Str(files) +
                "). Keep the program in the folder it came in.");
    SetFolder({});
    Centre();
}

void Frame::SetFolder(const fs::path &dir)
{
    folder = dir;
    folderText->SetLabel(dir.empty() ? wxString("Not unpacked yet.") : Wx("Unpacked: " + U8Str(dir.filename())));
    folderText->SetToolTip(dir.empty() ? wxString() : Wx(U8Str(dir)));
    if (!dir.empty() && out->IsEmpty())
    {
        std::string base = U8Str(dir.filename());
        const std::string suffix = " (unpacked)";
        if (base.size() > suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0)
            base.resize(base.size() - suffix.size());
        out->SetValue(Wx(U8Str(dir.parent_path() / U8Path(base + "-patched.ipa"))));
    }
    UpdateState();
}

void Frame::RefreshHyperspace()
{
    wxString text;
    if (!folder.empty())
        text = HasHyperspace(folder) ? "With Hyperspace: the mods include it; choose it below."
                                     : "Without Hyperspace: no mods with it applied.";
    if (hsText->GetLabel() != text)
    {
        hsText->SetLabel(text);
        hsText->GetContainingSizer()->Layout();
    }
}

void Frame::UpdateState()
{
    bool idle = !busy, have = !folder.empty() && IsUnpacked(folder);
    for (wxWindow *w : std::initializer_list<wxWindow *>{ipa, chooseIpa, unpackBtn, otherBtn, advanced, out, chooseOut,
                                                         hsDylib, chooseHs})
        w->Enable(idle);
    for (wxWindow *w : advancedWidgets) w->Enable(idle && advanced->IsChecked());
    openBtn->Enable(have);
    copyBtn->Enable(have);
    packBtn->Enable(idle && have);
    RefreshHyperspace();
}

void Frame::Run(std::function<void()> job)
{
    busy = true;
    UpdateState();
    gauge->Show();
    gauge->GetContainingSizer()->Layout();
    pulse.Start(80);
    std::thread(
        [this, job]
        {
            try
            {
                job();
            }
            catch (const std::exception &e)
            {
                LogLine(std::string("\nError: ") + e.what());
            }
            CallAfter(
                [this]
                {
                    busy = false;
                    pulse.Stop();
                    gauge->Hide();
                    UpdateState();
                });
        })
        .detach();
}

void Frame::OnChooseIpa(wxCommandEvent &)
{
    wxFileDialog d(this, "Choose the FTL IPA", "", "", "IPA (*.ipa)|*.ipa", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (d.ShowModal() == wxID_OK) ipa->SetValue(d.GetPath());
}

void Frame::OnUnpack(wxCommandEvent &)
{
    std::string path = Utf8(ipa->GetValue());
    if (path.empty())
        return void(wxMessageBox("Choose the FTL IPA first.", "FTL iOS Patcher", wxICON_INFORMATION, this));
    fs::path ipaPath = U8Path(path);
    if (!fs::is_regular_file(ipaPath))
        return void(wxMessageBox(Wx(path + " isn't a file."), "FTL iOS Patcher", wxICON_WARNING, this));
    fs::path dir = DefaultFolder(ipaPath);
    if (fs::exists(dir))
    {
        if (!IsUnpacked(dir))
            return void(wxMessageBox(Wx(U8Str(dir) + " already exists. Move it away or rename it first."),
                                     "FTL iOS Patcher", wxICON_WARNING, this));
        wxMessageDialog q(this, Wx("It was unpacked before, to\n" + U8Str(dir)), "FTL iOS Patcher",
                          wxYES_NO | wxCANCEL | wxICON_QUESTION);
        q.SetExtendedMessage("Unpacking again starts over and removes the mods applied to it.");
        q.SetYesNoCancelLabels("Use It", "Unpack Again", "Cancel");
        int c = q.ShowModal();
        if (c == wxID_CANCEL) return;
        if (c == wxID_YES)
        {
            SetFolder(dir);
            LogLine("Using " + U8Str(dir));
            return;
        }
    }
    out->Clear();
    Run(
        [this, ipaPath, dir]
        {
            Unpack(ipaPath, dir, [this](const std::string &s) { LogLine(s); });
            CallAfter([this, dir] { SetFolder(dir); });
            LogLine("\nNext: apply mods with ftlman (optional), then pack the IPA.");
        });
}

void Frame::OnOther(wxCommandEvent &)
{
    wxDirDialog d(this, "Choose a folder unpacked before", "", wxDD_DIR_MUST_EXIST);
    if (d.ShowModal() != wxID_OK) return;
    fs::path dir = U8Path(Utf8(d.GetPath()));
    if (!IsUnpacked(dir))
        return void(
            wxMessageBox(d.GetPath() + " wasn't unpacked by the patcher.", "FTL iOS Patcher", wxICON_WARNING, this));
    out->Clear();
    SetFolder(dir);
}

void Frame::OnPack(wxCommandEvent &)
{
    PackOptions opt;
    bool adv = advanced->IsChecked();  // the advanced fields count only while ticked
    opt.name = adv ? Utf8(name->GetValue().Strip(wxString::both)) : "";
    opt.bundleId = adv ? Utf8(bundle->GetValue().Strip(wxString::both)) : "";
    if (!opt.bundleId.empty() && !ValidBundleId(opt.bundleId))
        return void(wxMessageBox(
            Wx("\"" + opt.bundleId + "\" isn't a bundle ID: letters, digits, - and dots, like com.yourname.ftl"),
            "FTL iOS Patcher", wxICON_WARNING, this));
    if (adv && !icon->IsEmpty())
    {
        opt.icon = U8Path(Utf8(icon->GetValue()));
        if (!fs::is_regular_file(opt.icon))
            return void(wxMessageBox("The icon " + icon->GetValue() + " isn't there.", "FTL iOS Patcher",
                                     wxICON_WARNING, this));
    }
    if (!hsDylib->IsEmpty())
    {
        opt.hyperspace = U8Path(Utf8(hsDylib->GetValue()));
        try
        {
            CheckHyperspace(opt.hyperspace);
        }
        catch (const std::exception &e)
        {
            return void(wxMessageBox(Wx(e.what()), "FTL iOS Patcher", wxICON_WARNING, this));
        }
    }
    else if (HasHyperspace(folder) && !fs::is_regular_file(FindFiles() / HYPERSPACE_NAME))
        return void(wxMessageBox(Wx(std::string("The mods include Hyperspace. Choose Hyperspace's iOS build (") +
                                    HYPERSPACE_NAME + ") next to Hyperspace."),
                                 "FTL iOS Patcher", wxICON_INFORMATION, this));
    std::string o = Utf8(out->GetValue());
    if (o.empty())
        return void(wxMessageBox("Choose where to save the IPA.", "FTL iOS Patcher", wxICON_INFORMATION, this));
    fs::path outPath = U8Path(o);
    if (outPath.extension() != ".ipa") outPath += ".ipa";
    fs::path dir = folder, files = FindFiles();
    Run(
        [this, dir, files, outPath, opt]
        {
            LogLine("");
            Pack(dir, files, outPath, opt, [this](const std::string &s) { LogLine(s); });
        });
}

class App : public wxApp
{
public:
    bool OnInit() override
    {
#ifdef __WXMSW__
        MSWEnableDarkMode();  // follows Windows' dark mode
#endif
        SetAppDisplayName("FTL iOS Patcher");
        (new Frame())->Show();
        return true;
    }
};

}  // namespace

wxIMPLEMENT_APP_NO_MAIN(App);

#ifdef _WIN32
int GuiMain(int, char **)
{
    return wxEntry(GetModuleHandleW(nullptr), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
int GuiMain(int argc, char **argv)
{
    return wxEntry(argc, argv);
}
#endif
