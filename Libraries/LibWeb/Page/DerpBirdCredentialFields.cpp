#include <LibWeb/Page/DerpBirdCredentialFields.h>
#include <LibWeb/Bindings/CSS.h>

namespace Web {

ErrorOr<DerpBirdCredentialFields::SubmittedValues> DerpBirdCredentialFields::capture_form(HTML::HTMLFormElement& form, bool require_visible)
{
    auto& document = form.document();
    if (!form.is_connected() || !document.is_fully_active() || document.origin().is_opaque()
        || !document.url().scheme().is_one_of("http"sv, "https"sv))
        return Error::from_string_literal("Submitted form is unavailable");
    auto root = form.get_root_node();
    HTML::HTMLInputElement* current = nullptr;
    HTML::HTMLInputElement* replacement = nullptr;
    size_t visited = 0, password_count = 0;
    // Include controls associated with form= outside the form subtree. Bound
    // traversal before inspecting values; do not scan the document per field.
    for (auto* node = root.ptr(); node; node = node->next_in_pre_order(root.ptr())) {
        if (++visited > 65536)
            return Error::from_string_literal("Credential field scan exceeds limit");
        if (!is<HTML::HTMLInputElement>(*node)) continue;
        auto& input = as<HTML::HTMLInputElement>(*node);
        if (input.form() != &form || !eligible(input)
            || input.type_state() != HTML::HTMLInputElement::TypeAttributeState::Password
            || TRY(token(input, "one-time-code"sv))) continue;
        // Bound comparisons too: many matching, maximum-sized confirmation
        // fields must not turn the bounded DOM scan into gigabytes of work.
        if (++password_count > 8)
            return Error::from_string_literal("Too many submitted password fields");
        if (require_visible && !visible(input)) continue;
        if (input.value().length_in_code_units() > 65536)
            return Error::from_string_literal("Credential values exceed limits");
        if (TRY(token(input, "new-password"sv))) {
            if (replacement && replacement->value() != input.value())
                return Error::from_string_literal("New password confirmation differs");
            if (!replacement) replacement = &input;
        } else {
            if (current) return Error::from_string_literal("Ambiguous submitted passwords");
            current = &input;
        }
    }
    auto* chosen = replacement ? replacement : current;
    if (!chosen) return Error::from_string_literal("Submitted form has no password");
    auto fields = TRY(select_impl(*chosen, require_visible, true));
    auto values = TRY(fields.capture());
    if (values.password.is_empty())
        return Error::from_string_literal("Submitted password is empty");
    return SubmittedValues { fields.document_id(), fields.origin(), move(values), fields.kind() };
}

bool DerpBirdCredentialFields::visible(HTML::HTMLInputElement& input)
{
    DOM::Element::CheckVisibilityOptions options;
    options.check_opacity = true;
    options.check_visibility_css = true;
    options.content_visibility_auto = true;
    return input.check_visibility(options);
}

void DerpBirdCredentialFields::notify_input(HTML::HTMLInputElement& input)
{
    UIEvents::InputEventInit init;
    init.bubbles = true;
    init.composed = true;
    auto event = UIEvents::InputEvent::create_from_platform_event(HTML::EventNames::input, init, {},
        HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(input)));
    event->set_is_trusted(true);
    input.dispatch_event(*event);
}

}
