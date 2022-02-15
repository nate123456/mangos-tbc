using Microsoft.EntityFrameworkCore;
using Playerbot.Core.Abstractions;
using Playerbot.Core.Contexts;
using Playerbot.Core.Models;

namespace Playerbot.Core.Services;

public class PlayerbotRepository : IPlayerbotRepository
{
    private readonly PlayerbotDbContext _context;

    public PlayerbotRepository(PlayerbotDbContext context)
    {
        _context = context;
    }

    public async Task<PlayerbotToken?> GetTokenFromStringAsync(string tokenStr)
    {
        var token = await _context.Tokens
            .FirstOrDefaultAsync(t => t.Token == tokenStr.ToUpper());

        // no tokens allowed older than a day.
        if (token != null) return token.Age > DateTime.Now - TimeSpan.FromDays(1) ? token : null;

        return token;
    }

    public async Task<IEnumerable<PlayerbotScript>> GetScriptsAsync(int accountId)
    {
        var scripts = await _context.Scripts.Where(s => s.AccountId == accountId).ToListAsync();

        return scripts;
    }

    public async Task SetScriptsAsync(int accountId, IEnumerable<PlayerbotScript> scripts)
    {
        _context.Scripts.RemoveRange(_context.Scripts.Where(s => s.AccountId == accountId));

        await _context.Scripts.AddRangeAsync(scripts);
    }
}
