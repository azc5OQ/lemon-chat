# perl-extra-lib

Pure-Perl modules that OpenSSL's `Configure` and build scripts load and that the MSYS Perl of Git for
Windows does not ship: `ExtUtils::MakeMaker` and its `MM_*` back ends, `IPC::Cmd`,
`Module::Load::Conditional`, `Params::Check`, `Locale::Maketext::Simple` and the `Pod` tree
(`Pod::Usage`, `Pod::Text`, `Pod::Simple`, ...).

`_bundle_and_build_windows.sh` puts this directory on `PERL5LIB` by itself. Nothing here is used at
run time by the chat server; it exists only so the Windows stunnel build works with Git for Windows
and w64devkit alone.

Source: the `perl/lib` tree of Strawberry Perl 5.42.3.1 (64-bit portable). The modules are the
standard CPAN dual-life modules of a Perl distribution, licensed under the same terms as Perl itself
(Artistic License 1.0 or GNU GPL 1 or later).
