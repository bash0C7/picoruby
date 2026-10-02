MRuby::Gem::Specification.new('hal-task-darwin') do |spec|
  spec.license = 'MIT'
  spec.authors = 'bash0C7'
  spec.summary = 'Darwin HAL for mruby-task: the POSIX HAL with the tick delivered to the VM thread only'

  # External HAL provider for mruby-task (mruby's `hal-<short>-<conf>` naming):
  # when this gem is in the build, mruby-task's ports/posix/task_hal.c is
  # dropped and src/task_hal.c supplies the HAL. Depend on mruby-task for
  # task_hal.h.
  mruby_task = "#{MRUBY_ROOT}/mrbgems/picoruby-mruby/lib/mruby/mrbgems/mruby-task"
  spec.add_dependency 'mruby-task', gemdir: mruby_task
  spec.cc.include_paths << "#{mruby_task}/include"
end
