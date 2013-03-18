require "bundler/gem_tasks"
require "rspec/core/rake_task"

require "./lib/pacct/version.rb"

task :default => :spec

desc "Run specs"
RSpec::Core::RakeTask.new(:spec) do |task|
  task.rspec_opts =%w{--color --format progress}
  task.pattern = 'spec/*_spec.rb'
end

task :docs do
  system("rdoc ext lib")
end

desc "Build RPM"
task :rpm do
  Rake::Task['build'].invoke

  package = "pkg/pacct-#{Pacct::VERSION}-universal-linux.gem"

  FileUtils.cp(package, 'rpm/SOURCES')

  home_dir = Etc.getpwuid.dir

  rpmbuild_dir = File.join(home_dir, 'rpmbuild')
  spec_dir = File.join(rpmbuild_dir, 'SPECS')
  src_dir = File.join(rpmbuild_dir, 'SOURCES')

  [rpmbuild_dir, spec_dir, src_dir].each do |dir|
    FileUtils.mkdir_p(dir)
  end

  FileUtils.cp('rpm/pacct.spec', spec_dir)
  FileUtils.cp(package, src_dir)

  rake_dir = Dir.pwd

  Dir.chdir(rpmbuild_dir)

  system 'rpmbuild -ba SPECS/pacct.spec'

  Dir.chdir(rake_dir)
end

