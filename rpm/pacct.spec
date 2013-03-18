Name:		ruby-pacct
Version:	0.8.1
Release:	1%{?dist}
Summary:	A Ruby C extension for reading Linux process accounting files in acct(5) format

%define gem_name pacct
%define gem %{gem_name}-%{version}-universal-linux

Group:		Development/Libraries
License:	MIT
URL:		https://github.com/blm768/pacct
Source0:	%{gem}.gem
BuildRoot:	%(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

BuildRequires:	ruby-devel
Requires:	rubygems

%description
A Ruby C extension for reading Linux process accounting files in acct(5) format

%prep
#gem unpack %{SOURCE0}
#%setup -q -D -T -n %{gem}
%setup -q -D -T -n .
cp %{SOURCE0} .

#gem spec %{SOURCE0} -l --ruby > %{gem}.gemspec

%define gem_dir /usr/lib/ruby/gems/1.8
%define gem_instdir %{gem_dir}/gems/%{gem}

%build
mkdir -p ./%{gem_dir}

#gem build %{gem_name}.gemspec

export CONFIGURE_ARGS="--with-cflags='%{optflags}'"
gem install -V \
        --local \
        --install-dir ./%{gem_dir} \
        --bindir ./%{_bindir} \
        --force \
        --rdoc \
        %{gem}.gem

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}%{gem_instdir}

cp -a ./%{gem_dir}/* %{buildroot}%{gem_dir}/

#mkdir -p %{buildroot}%{gem_extdir_mri}/ext
#mv %{buildroot}%{gem_instdir}/lib/pacct/pacct_c.so %{buildroot}%{gem_extdir_mri}/ext

%clean
rm -rf %{buildroot}


%files
%defattr(-,root,root,-)
%doc %{gem_dir}/doc/%{gem}/*
%{gem_instdir}/*
%exclude %{gem_instdir}/.gitignore
%{gem_dir}/specifications/%{gem}.gemspec
%{gem_dir}/cache/%{gem}.gem
%exclude %{gem_dir}/bin/ruby_noexec_wrapper

%changelog

