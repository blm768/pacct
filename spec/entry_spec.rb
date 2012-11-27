require 'spec_helper'

describe Pacct::Entry do
  it "correctly converts integers to comp_t"
  
  it "raises an error when a comp_t overflows" do
    e = Pacct::Entry.new
    expect {
      e.memory = 1000000000000
    }.to raise_error("Exponent overflow in ulong_to_comp_t: Value 4000000000000 is too large.")
  end
end