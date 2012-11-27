require 'spec_helper'

describe Pacct::Entry do
  it "correctly converts \"comp_t\"s to integers" do
    comp_t_to_ulong = Pacct::Test.method(:comp_t_to_ulong)
    comp_t_to_ulong.call(1).should eql 1
    comp_t_to_ulong.call((1 << 13) | 1).should eql (1 << 3)
    comp_t_to_ulong.call((3 << 13) | 20).should eql (20 << 9)
  end

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
end