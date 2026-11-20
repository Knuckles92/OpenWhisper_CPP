#include "focus/uia_text_reader.h"

#include <windows.h>

// WIN32_LEAN_AND_MEAN leaves COM out of windows.h.
#include <ole2.h>

#include <UIAutomation.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include "focus/detail/wide.h"
#include "pyport/text.h"

namespace fv::focus {

namespace {

constexpr std::string_view kSource = "uia";

std::string text_of(IUIAutomationTextRange* range, int limit) {
    wil::unique_bstr value;
    THROW_IF_FAILED(range->GetText(limit, value.put()));
    if (!value) {
        return "";
    }
    return detail::narrow(std::wstring_view(value.get(), SysStringLen(value.get())));
}

wil::com_ptr<IUIAutomationTextRange> clone(IUIAutomationTextRange* range) {
    wil::com_ptr<IUIAutomationTextRange> copy;
    THROW_IF_FAILED(range->Clone(copy.put()));
    if (!copy) {
        THROW_HR(E_POINTER); // Clone returned no range.
    }
    return copy;
}

std::string text_before(IUIAutomationTextRange* anchor, std::size_t limit) {
    auto span = clone(anchor);
    THROW_IF_FAILED(span->MoveEndpointByRange(TextPatternRangeEndpoint_End, anchor,
                                              TextPatternRangeEndpoint_Start));
    int moved = 0;
    THROW_IF_FAILED(span->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character,
                                             -static_cast<int>(limit), &moved));
    // Some providers count a line break as one unit and return two characters for it, so read a
    // little more and keep the tail.
    return py::slice(text_of(span.get(), static_cast<int>(limit * 2)),
                     -static_cast<std::int64_t>(limit));
}

std::string text_after(IUIAutomationTextRange* anchor, std::size_t limit) {
    auto span = clone(anchor);
    THROW_IF_FAILED(span->MoveEndpointByRange(TextPatternRangeEndpoint_Start, anchor,
                                              TextPatternRangeEndpoint_End));
    int moved = 0;
    THROW_IF_FAILED(span->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character,
                                             static_cast<int>(limit), &moved));
    return text_of(span.get(), static_cast<int>(limit));
}

template <class Pattern>
wil::com_ptr<Pattern> pattern_of(IUIAutomationElement* element, PATTERNID id) {
    wil::com_ptr<Pattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(id, IID_PPV_ARGS(pattern.put())))) {
        return nullptr;
    }
    return pattern;
}

// The first selected range, and whether the selection could be read.
std::pair<wil::com_ptr<IUIAutomationTextRange>, bool>
selection_of(IUIAutomationTextPattern* pattern) {
    wil::com_ptr<IUIAutomationTextRangeArray> ranges;
    if (FAILED(pattern->GetSelection(ranges.put())) || !ranges) {
        return {nullptr, false};
    }
    int count = 0;
    if (FAILED(ranges->get_Length(&count))) {
        return {nullptr, false};
    }
    if (count < 1) {
        return {nullptr, true};
    }
    wil::com_ptr<IUIAutomationTextRange> first;
    if (FAILED(ranges->GetElement(0, first.put()))) {
        return {nullptr, false};
    }
    return {first, true};
}

wil::com_ptr<IUIAutomationTextRange> caret_of(IUIAutomationTextPattern2* pattern) {
    BOOL active = FALSE;
    wil::com_ptr<IUIAutomationTextRange> caret;
    if (FAILED(pattern->GetCaretRange(&active, caret.put()))) {
        return nullptr;
    }
    return caret;
}

std::optional<TextContext> read_element(IUIAutomationElement* element, bool include_text,
                                        bool include_selection) {
    BOOL password = FALSE;
    THROW_IF_FAILED(element->get_CurrentIsPassword(&password));
    if (password) {
        return TextContext{.source = std::string(kSource), .blocked = true};
    }
    auto pattern2 = pattern_of<IUIAutomationTextPattern2>(element, UIA_TextPattern2Id);
    const bool has_caret_range = pattern2 != nullptr;
    wil::com_ptr<IUIAutomationTextPattern> pattern;
    if (pattern2) {
        pattern = pattern2;
    } else {
        pattern = pattern_of<IUIAutomationTextPattern>(element, UIA_TextPatternId);
    }
    if (!pattern) {
        // Chromium builds its tree lazily: no TextPattern means unknown, never "empty".
        return std::nullopt;
    }
    // The selection also anchors the text around it, but its words are returned only to a
    // caller that asked for them.
    auto [anchor, selection_known] = selection_of(pattern.get());
    selection_known = selection_known && include_selection;
    if (!anchor && has_caret_range) {
        anchor = caret_of(pattern2.get());
    }
    if (!anchor) {
        return TextContext{.selection_known = selection_known, .source = std::string(kSource)};
    }
    std::string selected;
    if (selection_known) {
        selected = text_of(anchor.get(), static_cast<int>(MAX_SELECTION_CHARS + 1));
        if (py::len(selected) > MAX_SELECTION_CHARS) {
            selected.clear();
            selection_known = false;
        }
    }
    std::string before;
    std::string after;
    bool caret_known = false;
    if (include_text) {
        try {
            before = text_before(anchor.get(), CONTEXT_BEFORE_CHARS);
            after = text_after(anchor.get(), CONTEXT_AFTER_CHARS);
            caret_known = true;
        } catch (const wil::ResultException&) {
            before.clear();
            after.clear();
        }
    }
    return TextContext{.before = std::move(before),
                       .selected = std::move(selected),
                       .after = std::move(after),
                       .caret_known = caret_known,
                       .selection_known = selection_known,
                       .source = std::string(kSource)};
}

} // namespace

struct UiaTextReader::Impl {
    bool uninitialize = false;
    wil::com_ptr<IUIAutomation> automation;

    ~Impl() {
        automation.reset();
        if (uninitialize) {
            CoUninitialize();
        }
    }
};

UiaTextReader::UiaTextReader() : impl_(std::make_unique<Impl>()) {
    const HRESULT joined = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(joined) && joined != RPC_E_CHANGED_MODE) {
        THROW_HR(joined);
    }
    impl_->uninitialize = SUCCEEDED(joined);
    wil::com_ptr<IUIAutomation2> automation2;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(automation2.put()))) &&
        automation2) {
        THROW_IF_FAILED(
            automation2->put_ConnectionTimeout(static_cast<DWORD>(UIA_CONNECTION_TIMEOUT.count())));
        THROW_IF_FAILED(automation2->put_TransactionTimeout(
            static_cast<DWORD>(UIA_TRANSACTION_TIMEOUT.count())));
        impl_->automation = automation2;
        return;
    }
    // Before Windows 8 there is no IUIAutomation2 and so no timeouts; the capture deadline still
    // bounds the wait.
    THROW_IF_FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(impl_->automation.put())));
}

UiaTextReader::~UiaTextReader() = default;

std::optional<TextContext> UiaTextReader::read(const AppIdentity& identity, bool include_text,
                                               bool include_selection) {
    wil::com_ptr<IUIAutomationElement> element;
    THROW_IF_FAILED(impl_->automation->GetFocusedElement(element.put()));
    if (!element) {
        return std::nullopt;
    }
    int pid = 0;
    THROW_IF_FAILED(element->get_CurrentProcessId(&pid));
    // Our own windows answer UIA on the GUI thread, and focus that moved to another app since
    // identity was taken is not its text.
    const auto expected = identity.pid.value_or(0);
    if (static_cast<DWORD>(pid) == GetCurrentProcessId() ||
        (expected != 0 && static_cast<std::uint32_t>(pid) != expected)) {
        return std::nullopt;
    }
    return read_element(element.get(), include_text, include_selection);
}

std::optional<TextContext> UiaTextReader::read_window(std::uintptr_t window, bool include_text,
                                                      bool include_selection) {
    wil::com_ptr<IUIAutomationElement> element;
    THROW_IF_FAILED(
        impl_->automation->ElementFromHandle(reinterpret_cast<UIA_HWND>(window), element.put()));
    if (!element) {
        THROW_HR(E_POINTER); // No element for the window.
    }
    return read_element(element.get(), include_text, include_selection);
}

} // namespace fv::focus
