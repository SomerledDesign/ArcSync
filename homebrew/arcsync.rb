class Arcsync < Formula
  desc "Archive a Photos library to a hybrid CD/DVD ISO"
  homepage "https://github.com/SomerledDesign/ArcSync"
  url "https://github.com/SomerledDesign/ArcSync/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "MIT"
  depends_on :macos

  def install
    system "make", "PREFIX=#{prefix}", "install"
  end

  test do
    system "#{bin}/arcsync", "--version"
  end
end
