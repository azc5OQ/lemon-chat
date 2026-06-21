            function strToUtf8Bytes(str)
            {
                var utf8 = [];
                for (var i = 0; i < str.length; i++)
                {
                    var charcode = str.charCodeAt(i);
                    if (charcode < 0x80) utf8.push(charcode);
                    else if (charcode < 0x800)
                    {
                        utf8.push(0xc0 | (charcode >> 6),
                            0x80 | (charcode & 0x3f));
                    }
                    else if (charcode < 0xd800 || charcode >= 0xe000)
                    {
                        utf8.push(0xe0 | (charcode >> 12),
                            0x80 | ((charcode >> 6) & 0x3f),
                            0x80 | (charcode & 0x3f));
                    }
                    // surrogate pair
                    else
                    {
                        i++;
                        // UTF-16 encodes 0x10000-0x10FFFF by
                        // subtracting 0x10000 and splitting the
                        // 20 bits of 0x0-0xFFFFF into two halves
                        charcode = 0x10000 + (((charcode & 0x3ff) << 10) |
                            (str.charCodeAt(i) & 0x3ff));
                        utf8.push(0xf0 | (charcode >> 18),
                            0x80 | ((charcode >> 12) & 0x3f),
                            0x80 | ((charcode >> 6) & 0x3f),
                            0x80 | (charcode & 0x3f));
                    }
                }
                return utf8;
            }

            function bytesToUtf8String(data)
            { // array of bytes
                var str = '',
                    i;

                //we loop trough each element in array, each byte in this case
                for (i = 0; i < data.length; i++)
                {
                    var value = data[i];

                    if (value < 0x80)
                    {
                        str += String.fromCharCode(value);
                    }
                    else if (value > 0xBF && value < 0xE0)
                    {
                        str += String.fromCharCode((value & 0x1F) << 6 | data[i + 1] & 0x3F);
                        i += 1;
                    }
                    else if (value > 0xDF && value < 0xF0)
                    {
                        str += String.fromCharCode((value & 0x0F) << 12 | (data[i + 1] & 0x3F) << 6 | data[i + 2] & 0x3F);
                        i += 2;
                    }
                    else
                    {
                        // surrogate pair
                        var charCode = ((value & 0x07) << 18 | (data[i + 1] & 0x3F) << 12 | (data[i + 2] & 0x3F) << 6 | data[i + 3] & 0x3F) - 0x010000;

                        str += String.fromCharCode(charCode >> 10 | 0xD800, charCode & 0x03FF | 0xDC00);
                        i += 3;
                    }
                }

                return str;
            }

            // http://www.ietf.org/rfc/rfc2781.txt
            function bytesToUtf16String(w)
            {
                var i = 0;
                var len = w.length;
                var w1, w2;
                var charCodes = [];
                while (i < len)
                {
                    var w1 = w[i++];
                    if ((w1 & 0xF800) !== 0xD800)
                    { // w1 < 0xD800 || w1 > 0xDFFF
                        charCodes.push(w1);
                        continue;
                    }
                    if ((w1 & 0xFC00) === 0xD800)
                    { // w1 >= 0xD800 && w1 <= 0xDBFF
                        throw new RangeError('Invalid octet 0x' + w1.toString(16) + ' at offset ' + (i - 1));
                    }
                    if (i === len)
                    {
                        throw new RangeError('Expected additional octet');
                    }
                    w2 = w[i++];
                    if ((w2 & 0xFC00) !== 0xDC00)
                    { // w2 < 0xDC00 || w2 > 0xDFFF)
                        throw new RangeError('Invalid octet 0x' + w2.toString(16) + ' at offset ' + (i - 1));
                    }
                    charCodes.push(((w1 & 0x3ff) << 10) + (w2 & 0x3ff) + 0x10000);
                }
                return String.fromCharCode.apply(String, charCodes);
            }

            function strToUtf16Bytes(str)
            {
                const bytes = [];
                for (i = 0; i < str.length; i++)
                {
                    const code = str.charCodeAt(i); // x00-xFFFF
                    bytes.push(code & 255, code >> 8); // low, high
                }
                return bytes;
            }

            function bytesToBase64String(arr)
            {
                const abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; // base64 alphabet
                const bin = n => n.toString(2)
                    .padStart(8, 0); // convert num to 8-bit binary string
                const l = arr.length
                let result = '';

                for (let i = 0; i <= (l - 1) / 3; i++)
                {
                    let c1 = i * 3 + 1 >= l; // case when "=" is on end
                    let c2 = i * 3 + 2 >= l; // case when "=" is on end
                    let chunk = bin(arr[3 * i]) + bin(c1 ? 0 : arr[3 * i + 1]) + bin(c2 ? 0 : arr[3 * i + 2]);
                    let r = chunk.match(/.{1,6}/g)
                        .map((x, j) => j == 3 && c2 ? '=' : (j == 2 && c1 ? '=' : abc[+('0b' + x)]));
                    result += r.join('');
                }

                return result;
            }

            function base64StringToBytes(str)
            {
                const abc = [..."ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"]; // base64 alphabet
                let result = [];

                for (let i = 0; i < str.length / 4; i++)
                {
                    let chunk = [...str.slice(4 * i, 4 * i + 4)]
                    let bin = chunk.map(x => abc.indexOf(x)
                        .toString(2)
                        .padStart(6, 0))
                        .join('');
                    let bytes = bin.match(/.{1,8}/g)
                        .map(x => +('0b' + x));
                    result.push(...bytes.slice(0, 3 - (str[4 * i + 2] == "=") - (str[4 * i + 3] == "=")));
                }
                return result;
            }


            function substringByNullTerminator(str)
            {
                for (var x = 0; x < str.length; x++)
                {
                    if (str.charCodeAt(x) == 0)
                    {
                        return str.substring(0, x);
                    }
                }
                return str;
            }

            var sha256 = function sha256(ascii)
            {
                function rightRotate(value, amount)
                {
                    return (value >>> amount) | (value << (32 - amount));
                };

                var mathPow = Math.pow;
                var maxWord = mathPow(2, 32);
                var lengthProperty = 'length'
                var i, j; // Used as a counter across the whole file
                var result = ''

                var words = [];
                var asciiBitLength = ascii[lengthProperty] * 8;

                //* caching results is optional - remove/add slash from front of this line to toggle
                // Initial hash value: first 32 bits of the fractional parts of the square roots of the first 8 primes
                // (we actually calculate the first 64, but extra values are just ignored)
                var hash = sha256.h = sha256.h || [];
                // Round constants: first 32 bits of the fractional parts of the cube roots of the first 64 primes
                var k = sha256.k = sha256.k || [];
                var primeCounter = k[lengthProperty];
                /*/
                var hash = [], k = [];
                var primeCounter = 0;
                //*/

                var isComposite = {};
                for (var candidate = 2; primeCounter < 64; candidate++)
                {
                    if (!isComposite[candidate])
                    {
                        for (i = 0; i < 313; i += candidate)
                        {
                            isComposite[i] = candidate;
                        }
                        hash[primeCounter] = (mathPow(candidate, .5) * maxWord) | 0;
                        k[primeCounter++] = (mathPow(candidate, 1 / 3) * maxWord) | 0;
                    }
                }

                ascii += '\x80' // Append Æ‡' bit (plus zero padding)
                while (ascii[lengthProperty] % 64 - 56) ascii += '\x00' // More zero padding
                for (i = 0; i < ascii[lengthProperty]; i++)
                {
                    j = ascii.charCodeAt(i);
                    if (j >> 8) return; // ASCII check: only accept characters in range 0-255
                    words[i >> 2] |= j << ((3 - i) % 4) * 8;
                }
                words[words[lengthProperty]] = ((asciiBitLength / maxWord) | 0);
                words[words[lengthProperty]] = (asciiBitLength)

                // process each chunk
                for (j = 0; j < words[lengthProperty];)
                {
                    var w = words.slice(j, j += 16); // The message is expanded into 64 words as part of the iteration
                    var oldHash = hash;
                    // This is now the undefinedworking hash", often labelled as variables a...g
                    // (we have to truncate as well, otherwise extra entries at the end accumulate
                    hash = hash.slice(0, 8);

                    for (i = 0; i < 64; i++)
                    {
                        var i2 = i + j;
                        // Expand the message into 64 words
                        // Used below if
                        var w15 = w[i - 15],
                            w2 = w[i - 2];

                        // Iterate
                        var a = hash[0],
                            e = hash[4];
                        var temp1 = hash[7] +
                            (rightRotate(e, 6) ^ rightRotate(e, 11) ^ rightRotate(e, 25)) // S1
                            +
                            ((e & hash[5]) ^ ((~e) & hash[6])) // ch
                            +
                            k[i]
                            // Expand the message schedule if needed
                            +
                            (w[i] = (i < 16) ? w[i] : (
                                w[i - 16] +
                                (rightRotate(w15, 7) ^ rightRotate(w15, 18) ^ (w15 >>> 3)) // s0
                                +
                                w[i - 7] +
                                (rightRotate(w2, 17) ^ rightRotate(w2, 19) ^ (w2 >>> 10)) // s1
                            ) | 0);
                        // This is only used once, so *could* be moved below, but it only saves 4 bytes and makes things unreadble
                        var temp2 = (rightRotate(a, 2) ^ rightRotate(a, 13) ^ rightRotate(a, 22)) // S0
                            +
                            ((a & hash[1]) ^ (a & hash[2]) ^ (hash[1] & hash[2])); // maj

                        hash = [(temp1 + temp2) | 0].concat(hash); // We don't bother trimming off the extra ones, they're harmless as long as we're truncating when we do the slice()
                        hash[4] = (hash[4] + temp1) | 0;
                    }

                    for (i = 0; i < 8; i++)
                    {
                        hash[i] = (hash[i] + oldHash[i]) | 0;
                    }
                }

                for (i = 0; i < 8; i++)
                {
                    for (j = 3; j + 1; j--)
                    {
                        var b = (hash[i] >> (j * 8)) & 255;
                        result += ((b < 16) ? 0 : '') + b.toString(16);
                    }
                }
                return result;
            };
