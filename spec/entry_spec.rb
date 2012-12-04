require 'spec_helper'

describe Pacct::Entry do
  it "correctly converts \"comp_t\"s to integers" do
    comp_t_to_ulong = Pacct::Test.method(:comp_t_to_ulong)
    comp_t_to_ulong.call(1).should eql 1
    comp_t_to_ulong.call((1 << 13) | 1).should eql(1 << 3)
    comp_t_to_ulong.call((3 << 13) | 20).should eql(20 << 9)
  end

#To do: unit test this (eventually).
=begin
  it "correctly converts integers to \"comp_t\"s" do
    
  end
=end
  
  it "raises an error when a comp_t overflows" do
    e = Pacct::Entry.new
    expect {
      e.memory = 1000000000000
    }.to raise_error(RangeError, /Exponent overflow in ulong_to_comp_t: Value [\d]+ is too large./)
  end
  
  it "truncates command names if they become too long" do
    e = Pacct::Entry.new
    e.command_name = 'some_very_long_command_name'
    e.command_name.should eql 'some_very_long_'
  end
  
  it "raises an error when encountering unknown user/group IDs" do
    log = Pacct::Log.new('snapshot/pacct_invalid_ids')
    log.each_entry do |entry|
      #This assumes that these users and groups don't actually exist.
      #If, for some odd reason, they _do_ exist, this test will fail.
      expect { entry.user_name }.to raise_error(
        Errno::ENODATA.new('Unable to obtain user name for ID 4294967295').to_s)
      expect { entry.user_name = '_____ _' }.to raise_error(
         Errno::ENODATA.new("Unable to obtain user ID for name '_____ _'").to_s) 
      expect { entry.group_name }.to raise_error(
        Errno::ENODATA.new('Unable to obtain group name for ID 4294967295').to_s)
      expect { entry.group_name = '_____ _' }.to raise_error(
        Errno::ENODATA.new("Unable to obtain group ID for name '_____ _'").to_s) 
    end
  end
end