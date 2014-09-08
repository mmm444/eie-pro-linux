import sys

from xml.sax.handler import ContentHandler
import xml.sax
import xml.parsers.expat
import ConfigParser
import xml.sax

from collections import defaultdict

class Exact(xml.sax.handler.ContentHandler):
  def __init__(self):
    self.state = 0
    self.lens = []

  def startElement(self, name, attrs):
    if self.state == 0:
      if name == "field" and attrs['name'] == "usb.endpoint_number" and attrs['show'] == "0x81":
        self.state = 1
      elif name == "field" and attrs['name'] == "usb.endpoint_number" and attrs['show'] == "0x02":
        self.state = 2
    elif self.state == 1 and name == "field" and attrs['name'] == "usb.iso.data":
      print "In: " + attrs['show']
      self.state = -1
    elif self.state == 2 and name == "field" and attrs['name'] == "usb.win32.iso_data_len":
      self.lens.append(int(attrs['show'], 16))

  def endElement(self, name):
    if name == 'packet':
      if self.lens:
        d = defaultdict(int)
        s = ""
        for l in self.lens:
          s += str(l/12) + " "
          d[l] += 1

        print "Out: " + str(d) + s
        self.lens = []
      self.state = 0

if __name__ == '__main__':
  parser = xml.sax.make_parser()
  handler = Exact()
  parser.setContentHandler(handler)
  parser.parse(open(sys.argv[1]))
