#pragma once

#include <AK/Function.h>
#include <LibWebCommon/HTML/FileFilter.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebView/Utilities.h>
#include <QFileDialog>
#include <QMimeDatabase>
#include <QPointer>
#include <QStandardPaths>
#include <UI/Qt/StringUtils.h>

namespace Ladybird {

// The trusted shell has its own view; it does not inherit Tab's file-picker
// callback.
class DerpBirdFilePicker final : public QObject {
public:
    using Completion = Function<void(Vector<Web::HTML::SelectedFile>)>;
    explicit DerpBirdFilePicker(QWidget* parent)
        : QObject(parent)
        , m_parent(parent)
    {
    }
    ~DerpBirdFilePicker() override { dismiss(); }

    void open(Web::HTML::FileFilter const& accepted,
        Web::HTML::AllowMultipleFiles multiple, Completion completion)
    {
        if (m_dialog) {
            m_dialog->raise();
            m_dialog->activateWindow();
            return;
        }
        QStringList filters;
        QMimeDatabase database;
        for (auto const& filter : accepted.filters) {
            filter.visit(
                [&](Web::HTML::FileFilter::FileType type) {
                    QString prefix;
                    switch (type) {
                    case Web::HTML::FileFilter::FileType::Audio:
                        prefix = "audio/";
                        break;
                    case Web::HTML::FileFilter::FileType::Image:
                        prefix = "image/";
                        break;
                    case Web::HTML::FileFilter::FileType::Video:
                        prefix = "video/";
                        break;
                    }
                    QStringList extensions;
                    for (auto const& mime : database.allMimeTypes())
                        if (mime.name().startsWith(prefix))
                            extensions.append(mime.globPatterns());
                    filters.append(QString("%1 files (%2)")
                            .arg(prefix.chopped(1), extensions.join(' ')));
                },
                [&](Web::HTML::FileFilter::MimeType const& type) {
                    auto mime = database.mimeTypeForName(qstring_from_utf16_string(type.value));
                    if (mime.isValid())
                        filters.append(mime.filterString());
                },
                [&](Web::HTML::FileFilter::Extension const& extension) {
                    filters.append(QString("*.%1").arg(
                        qstring_from_utf16_string(extension.value)));
                });
        }
        filters.append("All files (*)");
        auto* dialog = new QFileDialog(
            m_parent,
            multiple == Web::HTML::AllowMultipleFiles::Yes ? "Select files"
                                                           : "Select file",
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
            filters.join(";;"));
        dialog->setObjectName("derpbird-file-picker");
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setFileMode(multiple == Web::HTML::AllowMultipleFiles::Yes
                ? QFileDialog::ExistingFiles
                : QFileDialog::ExistingFile);
        m_dialog = dialog;
        m_completion = move(completion);
        connect(dialog, &QDialog::finished, this,
            [this, dialog = QPointer<QFileDialog>(dialog)](int result) {
                if (!dialog || m_dialog != dialog)
                    return;
                Vector<Web::HTML::SelectedFile> files;
                if (result == QDialog::Accepted) {
                    for (auto const& path : dialog->selectedFiles()) {
                        auto file = WebView::create_selected_file(
                            ak_byte_string_from_qstring(path));
                        if (file.is_error())
                            warnln("Unable to open selected file: {}", file.error());
                        else
                            files.append(file.release_value());
                    }
                }
                m_dialog = nullptr;
                auto done = move(m_completion);
                if (done)
                    done(move(files));
            });
        dialog->open();
    }

    void cancel()
    {
        auto done = move(m_completion);
        dismiss();
        if (done)
            done({});
    }

    // Destruction or a renderer crash must not deliver a selection to a
    // replacement document.
    void dismiss()
    {
        m_completion = {};
        if (!m_dialog)
            return;
        auto dialog = m_dialog;
        m_dialog = nullptr;
        disconnect(dialog, nullptr, this, nullptr);
        dialog->close();
    }

private:
    QWidget* m_parent;
    QPointer<QFileDialog> m_dialog;
    Completion m_completion;
};

} // namespace Ladybird
