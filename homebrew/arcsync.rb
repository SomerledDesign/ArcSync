class Arcsync < Formula
  desc "Archive a Photos library to a hybrid CD/DVD ISO"
  homepage "https://github.com/SomerledDesign/ArcSync"
  url "https://github.com/SomerledDesign/ArcSync/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "#{SHA}"
  license "MIT"
  depends_on :macos

  def install
    system "make", "PREFIX=#{prefix}", "install"
  end

  test do
    assert_match "arcsync 1.0.0", shell_output("#{bin}/arcsync --version 2>&1")
    assert_path_exists man1/"arcsync.1"
  end
end
