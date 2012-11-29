require 'spec_helper'

describe Pacct do
  describe :CHECK_CALL do
    it "only raises an error if the expected result is not given" do
      Pacct::Test::check_call(0)
      expect { Pacct::Test::check_call(1) }.to raise_error(/1: result 0 expected, not 1 - pacct_c\.c\([\d]+\)/)
    end
    
    it "raises an error if errno is zero" do
      expect { Pacct::Test::check_call(2) }.to raise_error(/errno = 0: result 1 expected, not 0 - pacct_c\.c\([\d]+\)/)
    end
    
    #To consider: test for negative values in setters?
    
    it "raises an error if errno is nonzero" do
      expect { Pacct::Test::check_call(3) }.to raise_error(Errno::ERANGE, /Numerical result out of range - pacct_c\.c\([\d]+\)/)
    end
  end  
end
