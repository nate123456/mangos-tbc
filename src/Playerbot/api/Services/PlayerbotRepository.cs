using Microsoft.EntityFrameworkCore;
using Playerbot.Api.Abstractions;
using Playerbot.Api.Models;

namespace Playerbot.Api.Services;

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

    public void SetScripts(int accountId, IEnumerable<PlayerbotScript> scripts, bool isComplete)
    {
        var scriptsList = scripts.ToList();

        // populate in case they weren't populated by the client
        scriptsList.ForEach(s => s.AccountId = accountId);

        if (isComplete)
            _context.Scripts.RemoveRange(_context.Scripts.Where(s => s.AccountId == accountId));

        _context.UpdateRange(scriptsList);

        _context.SaveChanges();
    }

    public Task DeleteScriptsAsync(int accountId, IEnumerable<string> dtoScriptNames)
    {
        _context.Scripts.RemoveRange(_context.Scripts.Where(s =>
            s.AccountId == accountId && dtoScriptNames.Contains(s.Name)));

        return Task.CompletedTask;
    }
}
