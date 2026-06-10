# Append to homebrew-qtmesheditor/Casks/qtmesheditor.rb so Finder picks up
# CFBundleDocumentTypes without a reboot:
#
#   postflight do
#     system_command "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",
#       args: ["-f", "-R", "#{appdir}/QtMeshEditor.app"]
#   end
